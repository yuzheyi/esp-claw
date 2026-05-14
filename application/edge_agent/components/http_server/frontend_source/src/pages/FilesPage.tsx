import {
  ChevronRight,
  FolderOpen,
  HardDrive,
  HardDriveDownload,
  Trash2,
  Usb,
} from 'lucide-solid';
import { createEffect, createMemo, createSignal, For, onCleanup, Show, type Component } from 'solid-js';
import { t, tf } from '../i18n';
import {
  createFolder,
  deletePath,
  fetchFileContent,
  fetchFileList,
  fetchStorageDevices,
  mountStorage,
  unmountStorage,
  formatStorage,
  saveFileContent,
  uploadFile,
  type FileEntry,
  type StorageDevice,
} from '../api/client';
import { TabShell } from '../components/layout/TabShell';
import { PageHeader } from '../components/ui/PageHeader';
import { Button } from '../components/ui/Button';
import { Banner } from '../components/ui/Banner';
import { Modal } from '../components/ui/Modal';
import { markDirty } from '../state/dirty';
import { pushToast } from '../state/toast';

const EDITABLE_EXT = ['.md', '.json', '.txt', '.jsonl', '.jsonc', '.lua', '.log'];

function parentOf(path: string): string {
  if (path === '/') return '/';
  const parts = path.split('/').filter(Boolean);
  parts.pop();
  return parts.length ? '/' + parts.join('/') : '/';
}

function joinPath(base: string, name: string): string {
  return base === '/' ? '/' + name : base + '/' + name;
}

/** Parse virtualPath into (storage | null) and real API path. */
function getStorageAndPath(virtualPath: string): { storage: string | undefined; realPath: string } {
  if (virtualPath === '/' || virtualPath === '') {
    return { storage: undefined, realPath: '/' };
  }
  const seg = virtualPath.split('/')[1]; // first segment after /
  if (seg === 'fatfs') {
    return { storage: 'fatfs', realPath: virtualPath.slice('/fatfs'.length) || '/' };
  }
  if (seg === 'sdcard') {
    return { storage: 'sdcard', realPath: virtualPath.slice('/sdcard'.length) || '/' };
  }
  // unknown prefix → fall back to virtual root
  return { storage: undefined, realPath: '/' };
}

function humanSize(bytes: number): string {
  if (bytes < 1024) return bytes + ' B';
  if (bytes < 1024 * 1024) return (bytes / 1024).toFixed(1) + ' KB';
  return (bytes / (1024 * 1024)).toFixed(1) + ' MB';
}

function isEditable(path: string): boolean {
  const lower = path.toLowerCase();
  return EDITABLE_EXT.some((ext) => lower.endsWith(ext));
}

function downloadHref(path: string, storage?: string): string {
  const prefix = storage === 'sdcard' ? '/sdcard-files' : '/files';
  return prefix + path;
}

type EditorState = {
  open: boolean;
  path: string;
  content: string;
  baseline: string;
  loading: boolean;
  saving: boolean;
  error: string | null;
  readOnly: boolean;
};

const initialEditor: EditorState = {
  open: false,
  path: '',
  content: '',
  baseline: '',
  loading: false,
  saving: false,
  error: null,
  readOnly: true,
};

export const FilesPage: Component = () => {
  // Unified virtual path: "/" = virtual root, "/fatfs" = fatfs root, "/fatfs/sub" = fatfs subdir
  const [virtualPath, setVirtualPath] = createSignal('/');
  const [storageDevices, setStorageDevices] = createSignal<StorageDevice[]>([]);
  const [entries, setEntries] = createSignal<FileEntry[]>([]);
  const [error, setError] = createSignal<string | null>(null);
  const [loading, setLoading] = createSignal(false);
  const [devMode, setDevMode] = createSignal(false);
  const [uploadPath, setUploadPath] = createSignal('');
  const [fileChosenName, setFileChosenName] = createSignal<string | null>(null);
  const [newFolderName, setNewFolderName] = createSignal('');
  let fileInputRef: HTMLInputElement | undefined;
  const [chosenFile, setChosenFile] = createSignal<File | null>(null);

  const [editor, setEditor] = createSignal<EditorState>(initialEditor);
  const [uploadProgress, setUploadProgress] = createSignal<{
    name: string;
    loaded: number;
    total: number;
  } | null>(null);

  // Derived from virtualPath
  const parsedPath = createMemo(() => getStorageAndPath(virtualPath()));
  const isVirtualRoot = createMemo(() => parsedPath().storage === undefined);
  const currentStorage = createMemo(() => parsedPath().storage);
  const currentRealPath = createMemo(() => parsedPath().realPath);
  const pathSegments = createMemo(() => virtualPath().split('/').filter(Boolean));

  const dirtyEditor = () => editor().open && editor().content !== editor().baseline;

  createEffect(() => {
    markDirty('files', dirtyEditor());
  });
  onCleanup(() => markDirty('files', false));

  const loadList = async () => {
    setError(null);
    setLoading(true);
    try {
      const { storage, realPath } = parsedPath();
      if (storage === undefined) {
        // Virtual root – no files to list
        setEntries([]);
        setLoading(false);
        return;
      }
      const storageParam = storage !== 'fatfs' ? storage : undefined;
      const data = await fetchFileList(realPath, storageParam);
      setEntries(
        (data.entries ?? [])
          .slice()
          .sort((a, b) => Number(b.is_dir) - Number(a.is_dir) || a.name.localeCompare(b.name)),
      );
    } catch (err) {
      setError((err as Error).message);
    } finally {
      setLoading(false);
    }
  };

  const loadStorageDevices = async () => {
    try {
      const devices = await fetchStorageDevices();
      setStorageDevices(devices);
    } catch {
      /* ignore */
    }
  };

  createEffect(() => {
    void virtualPath();
    loadList();
  });

  /* Load storage device list on mount */
  loadStorageDevices();

  const toggleDevMode = () => {
    if (!devMode()) {
      if (!window.confirm(t('fileDevModeConfirm') as string)) return;
    }
    setDevMode(!devMode());
  };

  const handleUpload = async () => {
    if (!devMode()) {
      pushToast(t('fileDevModeRequired') as string, 'error');
      return;
    }
    const file = chosenFile();
    const target = uploadPath().trim() || (file ? joinPath(currentRealPath(), file.name) : '');
    if (!file || !target.startsWith('/')) {
      pushToast(t('fileSelectAndPath') as string, 'error');
      return;
    }
    /* Space pre-check */
    const currentDev = currentDevice();
    if (currentDev && currentDev.mounted && currentDev.free_bytes >= 0 && file.size > currentDev.free_bytes) {
      pushToast(
        tf('storageSpaceExceeded', {
          fileSize: humanSize(file.size),
          freeSpace: humanSize(currentDev.free_bytes),
        }),
        'error',
      );
      return;
    }
    try {
      const storage = currentStorage() !== 'fatfs' ? currentStorage() : undefined;
      setUploadProgress({ name: file.name, loaded: 0, total: file.size });
      await uploadFile(target, file, {
        storage,
        onProgress: (loaded, total) =>
          setUploadProgress((prev) => prev ? { ...prev, loaded, total } : prev),
      });
      setUploadProgress(null);
      setUploadPath('');
      setChosenFile(null);
      setFileChosenName(null);
      if (fileInputRef) fileInputRef.value = '';
      pushToast(t('fileUploadComplete') as string, 'success');
      await loadList();
      await loadStorageDevices();
    } catch (err) {
      setUploadProgress(null);
      pushToast((err as Error).message, 'error');
    }
  };

  const handleCreateFolder = async () => {
    if (!devMode()) {
      pushToast(t('fileDevModeRequired') as string, 'error');
      return;
    }
    const name = newFolderName().trim();
    if (!name) {
      pushToast(t('fileFolderNameRequired') as string, 'error');
      return;
    }
    try {
      const storage = currentStorage();
      if (!storage) {
        pushToast(t('fileSelectStorage') as string, 'error');
        return;
      }
      const storageParam = storage !== 'fatfs' ? storage : undefined;
      await createFolder(joinPath(currentRealPath(), name), { storage: storageParam });
      setNewFolderName('');
      pushToast(t('fileFolderCreated') as string, 'success');
      await loadList();
    } catch (err) {
      pushToast((err as Error).message, 'error');
    }
  };

  const handleDelete = async (entry: FileEntry) => {
    if (!devMode()) {
      pushToast(t('fileDevModeRequired') as string, 'error');
      return;
    }
    if (!window.confirm(tf('fileDeleteConfirm', { path: entry.path }))) return;
    try {
      const storage = currentStorage();
      const storageParam = storage && storage !== 'fatfs' ? storage : undefined;
      await deletePath(entry.path, storageParam);
      pushToast(t('fileDeleteComplete') as string, 'success');
      await loadList();
    } catch (err) {
      pushToast((err as Error).message, 'error');
    }
  };

  const openEditor = async (entry: FileEntry) => {
    setEditor({
      ...initialEditor,
      open: true,
      path: entry.path,
      loading: true,
      readOnly: !devMode(),
    });
    try {
      const storage = currentStorage() !== 'fatfs' ? currentStorage() : undefined;
      const { content } = await fetchFileContent(entry.path, { storage });
      setEditor((prev) => ({
        ...prev,
        content,
        baseline: content,
        loading: false,
      }));
    } catch (err) {
      setEditor((prev) => ({
        ...prev,
        error: (err as Error).message,
        loading: false,
      }));
    }
  };

  /** Navigate into a subdirectory inside the current storage. */
  const navigateIntoDir = (entry: FileEntry) => {
    const storage = currentStorage();
    if (!storage) return;
    setVirtualPath('/' + storage + entry.path);
  };

  const handleEntryActivate = async (entry: FileEntry) => {
    if (entry.is_dir) {
      navigateIntoDir(entry);
      return;
    }
    if (!isEditable(entry.path)) {
      pushToast(
        tf('fileUnsupportedAction', {
          action: devMode() ? (t('fileEdit') as string) : (t('filePreview') as string),
        }),
        'error',
      );
      return;
    }
    await openEditor(entry);
  };

  const openFolderFromAction = (entry: FileEntry) => {
    navigateIntoDir(entry);
  };

  const reloadEditor = async () => {
    const state = editor();
    if (!state.path) return;
    setEditor({ ...state, loading: true, error: null });
    try {
      const storage = currentStorage() !== 'fatfs' ? currentStorage() : undefined;
      const { content } = await fetchFileContent(state.path, { storage });
      setEditor((prev) => ({
        ...prev,
        content,
        baseline: content,
        loading: false,
      }));
    } catch (err) {
      setEditor((prev) => ({ ...prev, error: (err as Error).message, loading: false }));
    }
  };

  const saveEditor = async () => {
    const state = editor();
    if (!state.path) return;
    if (state.readOnly) {
      pushToast(t('fileDevModeRequired') as string, 'error');
      return;
    }
    setEditor({ ...state, saving: true, error: null });
    try {
      const storage = currentStorage() !== 'fatfs' ? currentStorage() : undefined;
      await saveFileContent(state.path, state.content, storage);
      setEditor((prev) => ({ ...prev, baseline: prev.content, saving: false }));
      pushToast(t('fileEditorSaved') as string, 'success');
      await loadList();
    } catch (err) {
      setEditor((prev) => ({ ...prev, saving: false, error: (err as Error).message }));
    }
  };

  const closeEditor = () => {
    if (dirtyEditor() && !window.confirm(t('unsavedConfirmLeave') as string)) {
      return;
    }
    setEditor(initialEditor);
  };

  const goUp = () => setVirtualPath(parentOf(virtualPath()));

  const currentDevice = () => {
    const storage = currentStorage();
    return storage ? storageDevices().find((d) => d.id === storage) : undefined;
  };

  const handleMount = async () => {
    try {
      await mountStorage(currentStorage()!);
      await loadStorageDevices();
      await loadList();
      pushToast(t('storageMountOk') as string, 'success');
    } catch (err) {
      pushToast(`${t('storageOpFailed')}: ${(err as Error).message}`, 'error');
    }
  };

  const handleUnmount = async () => {
    try {
      await unmountStorage(currentStorage()!);
      await loadStorageDevices();
      pushToast(t('storageUnmountOk') as string, 'success');
    } catch (err) {
      pushToast(`${t('storageOpFailed')}: ${(err as Error).message}`, 'error');
    }
  };

  const handleFormat = async () => {
    if (!window.confirm(t('storageFormatConfirm') as string)) return;
    try {
      await formatStorage(currentStorage()!);
      await loadStorageDevices();
      await loadList();
      pushToast(t('storageFormatOk') as string, 'success');
    } catch (err) {
      pushToast(`${t('storageOpFailed')}: ${(err as Error).message}`, 'error');
    }
  };

  return (
    <TabShell>
      <PageHeader
        title={t('navFiles') as string}
        actions={
          <div class="flex items-center gap-2 flex-wrap">
            <Show when={currentStorage() === 'sdcard'}>
              <Show when={currentDevice()?.mounted} fallback={
                <Button size="sm" variant="secondary" onClick={handleMount}>
                  {t('storageMount')}
                </Button>
              }>
                <Button size="sm" variant="secondary" onClick={handleUnmount}>
                  {t('storageUnmount')}
                </Button>
                <Button size="sm" variant="danger-ghost" onClick={handleFormat}>
                  {t('storageFormat')}
                </Button>
              </Show>
            </Show>
            <Show when={currentDevice()?.mounted && currentDevice()!.total_bytes > 0}>
              <span class="inline-flex items-center h-9 px-3 rounded-[var(--radius-sm)] border border-[var(--color-border-subtle)] bg-[var(--color-bg-surface)] text-[0.78rem] text-[var(--color-text-secondary)] whitespace-nowrap">
                {t('storageFree')}: {humanSize(currentDevice()!.free_bytes)} / {humanSize(currentDevice()!.total_bytes)}
              </span>
            </Show>
            <Button
              size="sm"
              variant="secondary"
              active={devMode()}
              onClick={toggleDevMode}
            >
              {devMode() ? t('fileDevModeOn') : t('fileDevMode')}
            </Button>
            <Button size="sm" variant="secondary" onClick={loadList} disabled={loading()}>
              {t('fileRefresh')}
            </Button>
          </div>
        }
      />
      <Show when={error()}>
        <div class="px-5 pt-4">
          <Banner kind="error" message={error() ?? undefined} />
        </div>
      </Show>
      {/* Breadcrumb navigation */}
      <div class="px-5 pt-4 flex flex-wrap items-center gap-1.5">
        <Button size="sm" variant="secondary" onClick={goUp} disabled={virtualPath() === '/'}>
          {t('fileUpDir')}
        </Button>
        <nav class="inline-flex items-center gap-1 flex-wrap" aria-label="Breadcrumb">
          <button
            type="button"
            class="inline-flex items-center h-8 px-2.5 rounded-[var(--radius-sm)] text-[0.82rem] font-mono text-[var(--color-text-primary)] hover:bg-white/[0.05] transition-colors"
            onClick={() => setVirtualPath('/')}
          >
            /
          </button>
          <Show when={virtualPath() !== '/'}>
            <For each={pathSegments()}>
              {(seg, i) => {
                const segs = pathSegments();
                const crumbPath = '/' + segs.slice(0, i() + 1).join('/');
                const isLast = i() === segs.length - 1;
                return (
                  <>
                    <ChevronRight class="h-3.5 w-3.5 text-[var(--color-text-muted)] shrink-0" />
                    <button
                      type="button"
                      class={`inline-flex items-center h-8 px-2.5 rounded-[var(--radius-sm)] text-[0.82rem] font-mono transition-colors ${isLast ? 'text-[var(--color-primary,rgba(232,54,45,0.85))] font-semibold' : 'text-[var(--color-text-secondary)] hover:bg-white/[0.05]'}`}
                      onClick={() => setVirtualPath(crumbPath)}
                    >
                      {seg}
                    </button>
                  </>
                );
              }}
            </For>
          </Show>
        </nav>
      </div>

      <Show when={devMode() && !isVirtualRoot()}>
        <div class="px-5 pt-3 flex flex-wrap gap-2 items-center">
          <input
            type="text"
            placeholder={t('fileNewFolder') as string}
            class="flex-1 min-w-[180px] max-w-sm text-[0.82rem] h-9"
            value={newFolderName()}
            onInput={(event) => setNewFolderName(event.currentTarget.value)}
          />
          <Button size="sm" variant="secondary" onClick={handleCreateFolder}>
            {t('fileCreateFolder')}
          </Button>
        </div>
        <div class="px-5 pt-3 flex flex-wrap gap-2 items-center">
          <input
            type="text"
            placeholder={t('fileUploadPath') as string}
            class="flex-1 min-w-[180px] max-w-sm text-[0.82rem] h-9"
            value={uploadPath()}
            onInput={(event) => setUploadPath(event.currentTarget.value)}
          />
          <div class="flex items-center gap-2 flex-1 min-w-[180px] h-9 px-2 rounded-[var(--radius-sm)] border border-[var(--color-border-subtle)] bg-[var(--color-bg-input)]">
            <input
              ref={fileInputRef}
              type="file"
              class="hidden"
              onChange={(event) => {
                const file = event.currentTarget.files?.[0] ?? null;
                setChosenFile(file);
                setFileChosenName(file?.name ?? null);
                if (file) {
                  setUploadPath(joinPath(currentRealPath(), file.name));
                }
              }}
            />
            <Button variant="ghost" size="xs" onClick={() => fileInputRef?.click()}>
              {t('fileChoose')}
            </Button>
            <span class="text-[0.82rem] text-[var(--color-text-muted)] flex-1 truncate">
              {fileChosenName() ?? t('fileNoFileSelected')}
            </span>
          </div>
          <Button size="sm" variant="primary" onClick={handleUpload} disabled={!!uploadProgress()}>
            {t('fileUpload')}
          </Button>
        </div>
        <Show when={uploadProgress()}>
          <div class="px-5 pt-2">
            <div class="rounded-[var(--radius-sm)] border border-[var(--color-border-subtle)] bg-[var(--color-bg-surface)] p-3">
              <div class="flex items-center justify-between text-[0.82rem] text-[var(--color-text-secondary)] mb-1.5">
                <span class="truncate mr-2">{uploadProgress()!.name}</span>
                <span class="whitespace-nowrap">
                  {humanSize(uploadProgress()!.loaded)} / {humanSize(uploadProgress()!.total)}
                  {' ('}{uploadProgress()!.total > 0
                    ? `${Math.round((uploadProgress()!.loaded / uploadProgress()!.total) * 100)}%`
                    : '--'}{')'}
                </span>
              </div>
              <div class="h-2 rounded-full bg-[var(--color-bg-input)] overflow-hidden">
                <div
                  class="h-full rounded-full bg-[var(--color-primary,rgba(232,54,45,0.85))] transition-[width] duration-150 ease-linear"
                  style={{ width: `${uploadProgress()!.total > 0
                    ? (uploadProgress()!.loaded / uploadProgress()!.total) * 100
                    : 0}%` }}
                />
              </div>
            </div>
          </div>
        </Show>
      </Show>

      <div class="p-5">
        <Show
          when={!isVirtualRoot()}
          fallback={
            /* Virtual root: show storage mount points */
            <div class="grid grid-cols-1 sm:grid-cols-2 gap-4">
              <For each={storageDevices()}>
                {(dev) => {
                  const devVirtualPath = '/' + dev.id;
                  return (
                    <button
                      type="button"
                      class="group flex items-start gap-4 p-5 rounded-[var(--radius-md)] border border-[var(--color-border-subtle)] bg-[var(--color-bg-surface)] hover:border-[var(--color-primary,rgba(232,54,45,0.4))] transition-colors text-left"
                      onClick={() => {
                        if (dev.mounted) {
                          setVirtualPath(devVirtualPath);
                        }
                      }}
                      disabled={!dev.mounted}
                    >
                      <div class={`mt-0.5 flex items-center justify-center h-10 w-10 rounded-[var(--radius-sm)] shrink-0 ${dev.mounted ? 'bg-[rgba(232,54,45,0.08)] text-[var(--color-primary,rgba(232,54,45,0.85))]' : 'bg-[var(--color-bg-card)] text-[var(--color-text-secondary)]'}`}>
                        <Show when={dev.id === 'sdcard'} fallback={<HardDrive class="h-5 w-5" />}>
                          <Usb class="h-5 w-5" />
                        </Show>
                      </div>
                      <div class="flex-1 min-w-0">
                        <div class="flex items-center gap-2">
                          <span class="text-[0.92rem] font-medium text-[var(--color-text-primary)] truncate">
                            {dev.name}
                          </span>
                          <Show when={!dev.mounted}>
                            <span class="text-[0.72rem] px-1.5 py-0.5 rounded-[var(--radius-sm)] bg-[var(--color-bg-card)] text-[var(--color-text-muted)]">
                              {t('storageNotMounted')}
                            </span>
                          </Show>
                        </div>
                        <code class="text-[0.78rem] text-[var(--color-text-muted)] font-mono">
                          {devVirtualPath}
                        </code>
                        <Show when={dev.mounted && dev.total_bytes > 0}>
                          <div class="mt-2">
                            <div class="flex items-center justify-between text-[0.75rem] text-[var(--color-text-secondary)] mb-1">
                              <span>{humanSize(dev.free_bytes)} {t('storageFree')}</span>
                              <span>{humanSize(dev.total_bytes)}</span>
                            </div>
                            <div class="h-1.5 rounded-full bg-[var(--color-bg-input)] overflow-hidden">
                              <div
                                class="h-full rounded-full bg-[var(--color-primary,rgba(232,54,45,0.85))]"
                                style={{ width: `${Math.max(0, Math.min(100, ((dev.total_bytes - dev.free_bytes) / dev.total_bytes) * 100))}%` }}
                              />
                            </div>
                          </div>
                        </Show>
                      </div>
                    </button>
                  );
                }}
              </For>
            </div>
          }
        >
          {/* Normal file list inside a storage */}
          <div class="rounded-[var(--radius-md)] border border-[var(--color-border-subtle)] bg-[var(--color-bg-surface)] overflow-hidden">
          <table class="w-full">
            <thead>
              <tr class="bg-[var(--color-bg-card)]">
                <th class="text-left px-4 py-2.5 text-[0.72rem] font-bold uppercase tracking-wider text-[var(--color-text-muted)]">{t('fileColName')}</th>
                <th class="text-left px-4 py-2.5 text-[0.72rem] font-bold uppercase tracking-wider text-[var(--color-text-muted)]">{t('fileColType')}</th>
                <th class="text-left px-4 py-2.5 text-[0.72rem] font-bold uppercase tracking-wider text-[var(--color-text-muted)]">{t('fileColSize')}</th>
                <th class="text-left px-4 py-2.5 text-[0.72rem] font-bold uppercase tracking-wider text-[var(--color-text-muted)]">{t('fileColActions')}</th>
              </tr>
            </thead>
            <tbody>
              <Show
                when={entries().length > 0}
                fallback={
                  <tr>
                    <td colspan={4} class="px-4 py-6 text-center text-[var(--color-text-muted)]">
                      {t('fileEmpty')}
                    </td>
                  </tr>
                }
              >
                <For each={entries()}>
                  {(entry) => (
                    <tr class="border-t border-[var(--color-border-subtle)] hover:bg-white/[0.02]">
                      <td
                        class="px-4 py-2.5 text-[0.88rem] text-[var(--color-text-primary)] break-all cursor-pointer"
                        onClick={() => void handleEntryActivate(entry)}
                      >
                        {entry.name}
                      </td>
                      <td
                        class="px-4 py-2.5 text-[0.82rem] text-[var(--color-text-secondary)] cursor-pointer"
                        onClick={() => void handleEntryActivate(entry)}
                      >
                        {entry.is_dir ? t('fileTypeFolder') : t('fileTypeFile')}
                      </td>
                      <td
                        class="px-4 py-2.5 text-[0.82rem] text-[var(--color-text-secondary)] cursor-pointer"
                        onClick={() => void handleEntryActivate(entry)}
                      >
                        {entry.is_dir ? '—' : humanSize(entry.size ?? 0)}
                      </td>
                      <td class="px-4 py-2.5">
                        <div class="flex flex-wrap items-center gap-1.5" onClick={(event) => event.stopPropagation()}>
                          <Show
                            when={entry.is_dir}
                            fallback={
                              <a
                                href={downloadHref(entry.path, currentStorage())}
                                target="_blank"
                                rel="noopener"
                                class="inline-flex h-8 w-8 items-center justify-center rounded-[var(--radius-sm)] text-white hover:bg-white/[0.05]"
                                title={t('fileDownload') as string}
                                aria-label={t('fileDownload') as string}
                              >
                                <HardDriveDownload class="h-4 w-4 shrink-0" />
                              </a>
                            }
                          >
                            <button
                              type="button"
                              class="inline-flex h-8 w-8 items-center justify-center rounded-[var(--radius-sm)] text-[var(--color-text-primary)] hover:bg-white/[0.05]"
                              onClick={() => openFolderFromAction(entry)}
                              title={t('fileOpen') as string}
                              aria-label={t('fileOpen') as string}
                            >
                              <FolderOpen class="h-4 w-4 shrink-0" />
                            </button>
                          </Show>
                          <button
                            type="button"
                            class="inline-flex h-8 w-8 items-center justify-center rounded-[var(--radius-sm)] text-[var(--color-danger)] hover:bg-[rgba(248,113,113,0.08)] disabled:cursor-not-allowed disabled:opacity-50"
                            disabled={!devMode()}
                            onClick={() => void handleDelete(entry)}
                            title={t('fileDelete') as string}
                            aria-label={t('fileDelete') as string}
                          >
                            <Trash2 class="h-4 w-4 shrink-0" />
                          </button>
                        </div>
                      </td>
                    </tr>
                  )}
                </For>
              </Show>
            </tbody>
          </table>
        </div>
        </Show>
      </div>

      <FileEditorModal
        state={editor}
        onClose={closeEditor}
        onContentChange={(value) =>
          setEditor((prev) => ({ ...prev, content: value }))
        }
        onReload={reloadEditor}
        onSave={saveEditor}
      />
    </TabShell>
  );
};

const FileEditorModal: Component<{
  state: () => EditorState;
  onClose: () => void;
  onContentChange: (value: string) => void;
  onReload: () => void;
  onSave: () => void;
}> = (props) => {
  return (
    <Modal
      open={props.state().open}
      onClose={props.onClose}
      title={t('fileEditorTitle') as string}
      subtitle={<code class="font-mono text-[0.8rem] text-[var(--color-text-muted)]">{props.state().path}</code>}
      actions={
        <>
          <Button size="sm" variant="secondary" onClick={props.onReload} disabled={props.state().loading}>
            {t('fileEditorRefresh')}
          </Button>
          <Show when={!props.state().readOnly}>
            <Button size="sm" variant="primary" onClick={props.onSave} disabled={props.state().saving}>
              {props.state().saving ? '…' : t('fileEditorSave')}
            </Button>
          </Show>
        </>
      }
    >
      <div class="p-5 flex flex-col gap-3">
        <Show when={props.state().error}>
          <Banner kind="error" message={props.state().error ?? undefined} />
        </Show>
        <textarea
          spellcheck={false}
          readonly={props.state().readOnly}
          class="w-full min-h-[420px] rounded-[var(--radius-md)] border border-[var(--color-border-subtle)] bg-[var(--color-bg-input)] p-4 text-[0.88rem] leading-6 font-mono text-[var(--color-text-primary)] focus:outline-none focus:border-[rgba(232,54,45,0.4)]"
          value={props.state().content}
          onInput={(event) => props.onContentChange(event.currentTarget.value)}
        />
      </div>
    </Modal>
  );
};
