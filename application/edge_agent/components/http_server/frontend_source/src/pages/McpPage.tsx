import {
  createMemo,
  createSignal,
  For,
  onMount,
  Show,
  type Component,
} from 'solid-js';
import { t } from '../i18n';
import { TabShell } from '../components/layout/TabShell';
import { PageHeader } from '../components/ui/PageHeader';
import { TextInput } from '../components/ui/FormField';
import { Button } from '../components/ui/Button';
import { pushToast } from '../state/toast';

/* ── Types ─────────────────────────────────────────── */

type McpServer = {
  name: string;
  url: string;
  token: string;
  endpoint: string;
  description: string;
  enabled: boolean;
};

type ModalMode = 'add' | 'edit';

/* ── API helpers ────────────────────────────────────── */

const API = '/api/mcp_servers';

async function fetchServers(): Promise<McpServer[]> {
  try {
    const res = await fetch(API);
    const data = await res.json();
    return data.servers ?? [];
  } catch {
    return [];
  }
}

async function apiPost(body: Record<string, unknown>): Promise<{ error?: string } & Record<string, unknown>> {
  try {
    const res = await fetch(API, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(body),
    });
    return await res.json();
  } catch (e) {
    return { error: String(e) };
  }
}

/* ── Helpers ────────────────────────────────────────── */

function detectType(ep: string): 'http' | 'sse' {
  return ep.startsWith('/sse') || ep === 'sse' ? 'sse' : 'http';
}

/* ── Component ──────────────────────────────────────── */

export const McpPage: Component = () => {
  const [servers, setServers] = createSignal<McpServer[]>([]);
  const [loading, setLoading] = createSignal(true);
  const [search, setSearch] = createSignal('');
  const [testing, setTesting] = createSignal<Record<string, 'idle' | 'checking' | 'online' | 'offline'>>({});

  /* Modal state */
  const [showModal, setShowModal] = createSignal(false);
  const [modalMode, setModalMode] = createSignal<ModalMode>('add');
  const [editOrigName, setEditOrigName] = createSignal('');
  /* form fields */
  const [fName, setFName] = createSignal('');
  const [fDesc, setFDesc] = createSignal('');
  const [fType, setFType] = createSignal<'http' | 'sse'>('http');
  const [fUrl, setFUrl] = createSignal('');
  const [fToken, setFToken] = createSignal('');
  const [fEndpoint, setFEndpoint] = createSignal('mcp');
  const [fSaving, setFSaving] = createSignal(false);

  const filtered = createMemo(() => {
    const q = search().toLowerCase();
    if (!q) return servers();
    return servers().filter(
      (s) => s.name.toLowerCase().includes(q) || s.url.toLowerCase().includes(q) || s.description.toLowerCase().includes(q),
    );
  });

  const load = async () => {
    setLoading(true);
    setServers(await fetchServers());
    setLoading(false);
  };

  onMount(() => {
    load();
  });

  /* ── Modal open helpers ──────────────────────────── */

  const openAdd = () => {
    setModalMode('add');
    setEditOrigName('');
    setFName('');
    setFDesc('');
    setFType('http');
    setFUrl('');
    setFToken('');
    setFEndpoint('mcp');
    setShowModal(true);
  };

  const openEdit = (s: McpServer) => {
    setModalMode('edit');
    setEditOrigName(s.name);
    setFName(s.name);
    setFDesc(s.description);
    setFType(detectType(s.endpoint));
    setFUrl(s.url);
    setFToken(s.token === '***' ? '' : s.token);
    setFEndpoint(s.endpoint);
    setShowModal(true);
  };

  const closeModal = () => {
    setShowModal(false);
  };

  /* ── Actions ──────────────────────────────────────── */

  const handleSave = async () => {
    if (!fName() || !fUrl()) {
      pushToast(t('mcpNameUrlRequired') as string, 'error');
      return;
    }
    setFSaving(true);
    const ep = fType() === 'sse' && !fEndpoint().startsWith('/sse') ? '/sse' : fEndpoint();
    const body: Record<string, unknown> = {
      action: modalMode() === 'add' ? 'add' : 'edit',
      name: fName(),
      url: fUrl(),
      token: fToken(),
      endpoint: ep,
      description: fDesc(),
    };
    if (modalMode() === 'edit' && editOrigName() !== fName()) {
      /* Name changed: remove old, add new */
      await apiPost({ action: 'remove', name: editOrigName() });
    }
    const data = await apiPost(body);
    setFSaving(false);
    if (data.error) {
      pushToast(data.error as string, 'error');
      return;
    }
    pushToast(modalMode() === 'add' ? (t('mcpAddOk') as string) : (t('mcpEditTitle') as string) + ' ✓', 'success');
    closeModal();
    await load();
  };

  const handleRemove = async (name: string) => {
    if (!confirm((t('mcpConfirmDelete') as string).replace('{name}', name))) return;
    const data = await apiPost({ action: 'remove', name });
    if (data.error) { pushToast(data.error as string, 'error'); return; }
    pushToast(t('mcpRemoveOk') as string, 'success');
    await load();
  };

  const handleToggle = async (name: string) => {
    const data = await apiPost({ action: 'toggle', name });
    if (data.error) { pushToast(data.error as string, 'error'); return; }
    await load();
  };

  const handleTest = async (name: string) => {
    setTesting((prev) => ({ ...prev, [name]: 'checking' }));
    const data = await apiPost({ action: 'verify', name });
    if (data.error) {
      setTesting((prev) => ({ ...prev, [name]: 'offline' }));
      pushToast(`${name}: ${data.error as string}`, 'error');
      return;
    }
    const online = data.reachable === true;
    const detail = (data.detail as string) || '';
    setTesting((prev) => ({ ...prev, [name]: online ? 'online' : 'offline' }));
    pushToast(`${name}: ${online ? '✔' : '✘'} ${detail}`, online ? 'success' : 'error');
  };

  const handleCopyUrl = async (url: string) => {
    try {
      await navigator.clipboard.writeText(url);
      pushToast(t('mcpCopyOk') as string, 'success');
    } catch {
      pushToast('Copy failed', 'error');
    }
  };

  /* ── Modal form ───────────────────────────────────── */

  const ModalForm = () => (
    <div class="fixed inset-0 z-50 flex items-center justify-center bg-black/60 backdrop-blur-sm" onClick={closeModal}>
      <div class="bg-[var(--color-bg-card)] border border-[var(--color-border-subtle)] rounded-[var(--radius-lg)] shadow-2xl w-full max-w-lg mx-4 max-h-[90vh] overflow-y-auto" onClick={(e) => e.stopPropagation()}>
        {/* Header */}
        <div class="flex items-center justify-between px-6 py-4 border-b border-[var(--color-border-subtle)]">
          <h3 class="text-base font-semibold text-[var(--color-text-primary)] m-0">
            {modalMode() === 'add' ? t('mcpAddTitle') : t('mcpEditTitle')}
          </h3>
          <button class="text-[var(--color-text-muted)] hover:text-[var(--color-text-primary)] text-xl leading-none bg-transparent border-none cursor-pointer p-1" onClick={closeModal}>&times;</button>
        </div>

        {/* Body */}
        <div class="px-6 py-5 space-y-4">
          {/* Name */}
          <TextInput
            label={t('mcpName') as string}
            placeholder="e.g.: my-mcp-server"
            value={fName()}
            onInput={(e) => setFName(e.currentTarget.value)}
          />

          {/* Description */}
          <TextInput
            label={t('mcpDescription') as string}
            placeholder={t('mcpDescPlaceholder') as string}
            value={fDesc()}
            onInput={(e) => setFDesc(e.currentTarget.value)}
          />

          {/* Type selector */}
          <div class="flex flex-col gap-1.5">
            <label class="text-[0.8rem] text-[var(--color-text-secondary)] font-medium">{t('mcpType')}</label>
            <div class="flex gap-3">
              <label class="flex items-center gap-2 cursor-pointer">
                <input type="radio" name="mcp-type" checked={fType() === 'http'}
                  onChange={() => setFType('http')} class="accent-[var(--color-accent)]" />
                <span class="text-sm text-[var(--color-text-primary)]">{t('mcpTypeHttp')}</span>
              </label>
              <label class="flex items-center gap-2 cursor-pointer">
                <input type="radio" name="mcp-type" checked={fType() === 'sse'}
                  onChange={() => setFType('sse')} class="accent-[var(--color-accent)]" />
                <span class="text-sm text-[var(--color-text-primary)]">{t('mcpTypeSse')}</span>
              </label>
            </div>
          </div>

          {/* URL */}
          <TextInput
            label={t('mcpUrl') as string}
            placeholder="http://host:port/path"
            value={fUrl()}
            onInput={(e) => setFUrl(e.currentTarget.value)}
          />

          {/* Endpoint */}
          <TextInput
            label={t('mcpEndpoint') as string}
            placeholder={fType() === 'sse' ? '/sse' : 'mcp'}
            value={fEndpoint()}
            onInput={(e) => setFEndpoint(e.currentTarget.value)}
          />

          {/* Token */}
          <TextInput
            label={t('mcpToken') as string}
            placeholder={t('mcpTokenPlaceholder') as string}
            type="password"
            value={fToken()}
            onInput={(e) => setFToken(e.currentTarget.value)}
          />
        </div>

        {/* Footer */}
        <div class="flex items-center justify-end gap-3 px-6 py-4 border-t border-[var(--color-border-subtle)]">
          <Button variant="secondary" onClick={closeModal}>{t('mcpCancel')}</Button>
          <Button variant="primary" onClick={handleSave} disabled={fSaving()}>
            {fSaving() ? (t('mcpSaving') as string) : (t('mcpSaveBtn') as string)}
          </Button>
        </div>
      </div>
    </div>
  );

  /* ── Status badge ──────────────────────────────────── */

  const StatusBadge = (props: { name: string; enabled: boolean }) => {
    const st = createMemo(() => testing()[props.name] ?? 'idle');
    const color = () => {
      if (st() === 'online') return 'text-green-400 border-green-500/40 bg-green-500/10';
      if (st() === 'offline') return 'text-red-400 border-red-500/40 bg-red-500/10';
      if (st() === 'checking') return 'text-yellow-400 border-yellow-500/40 bg-yellow-500/10';
      if (!props.enabled) return 'text-gray-400 border-gray-500/40 bg-gray-500/10';
      return 'text-gray-500 border-gray-600/30 bg-gray-600/10';
    };
    const label = () => {
      if (st() === 'checking') return t('mcpChecking');
      if (st() === 'online') return t('mcpOnline');
      if (st() === 'offline') return t('mcpOffline');
      if (!props.enabled) return t('mcpDisabled');
      return '—';
    };
    return (
      <span class={`inline-flex items-center gap-1.5 px-2.5 py-0.5 rounded-full text-[0.72rem] font-medium border ${color()}`}>
        <span class={`w-1.5 h-1.5 rounded-full ${
          st() === 'online' ? 'bg-green-400' :
          st() === 'offline' ? 'bg-red-400' :
          st() === 'checking' ? 'bg-yellow-400 animate-pulse' :
          !props.enabled ? 'bg-gray-400' : 'bg-gray-600'
        }`} />
        {label()}
      </span>
    );
  };

  /* ── Render ────────────────────────────────────────── */

  return (
    <TabShell>
      {/* Page header with toolbar */}
      <PageHeader
        title={t('navMcp') as string}
        description={t('mcpDesc') as string}
        actions={
          <div class="flex items-center gap-2">
            <Button variant="secondary" size="sm" onClick={load} disabled={loading()}>
              {loading() ? (t('mcpRefreshing') as string) : (t('mcpRefresh') as string)}
            </Button>
            <Button variant="primary" size="sm" onClick={openAdd}>
              + {t('mcpAddBtn')}
            </Button>
          </div>
        }
      />

      <div class="p-5 space-y-4">
        {/* Search */}
        <div class="relative">
          <input
            type="text"
            value={search()}
            onInput={(e) => setSearch(e.currentTarget.value)}
            placeholder={t('mcpSearchPlaceholder') as string}
            class="w-full max-w-xs rounded-[var(--radius-sm)] border border-[var(--color-border-subtle)] bg-[var(--color-bg-input)] px-3 py-1.5 text-sm text-[var(--color-text-primary)] placeholder:text-[var(--color-text-muted)] outline-none focus:border-[var(--color-accent-soft)] transition"
          />
        </div>

        {/* Server list */}
        <Show when={!loading()} fallback={<div class="text-center py-12 text-[var(--color-text-muted)] text-sm">{t('statusLoading')}</div>}>
          <Show when={filtered().length > 0} fallback={
            <div class="text-center py-12 text-[var(--color-text-muted)] text-sm">
              {search() ? 'No matching servers' : t('mcpEmpty')}
            </div>
          }>
            {/* Table */}
            <div class="overflow-x-auto rounded-[var(--radius-sm)] border border-[var(--color-border-subtle)]">
              <table class="w-full text-left text-sm border-collapse">
                <thead>
                  <tr class="border-b border-[var(--color-border-subtle)] bg-[var(--color-bg-surface)]/40">
                    <th class="py-3 px-4 text-[0.75rem] font-semibold uppercase tracking-wider text-[var(--color-text-muted)]">{t('mcpName')}</th>
                    <th class="py-3 px-4 text-[0.75rem] font-semibold uppercase tracking-wider text-[var(--color-text-muted)]">{t('mcpUrl')}</th>
                    <th class="py-3 px-4 text-[0.75rem] font-semibold uppercase tracking-wider text-[var(--color-text-muted)]">{t('mcpType')}</th>
                    <th class="py-3 px-4 text-[0.75rem] font-semibold uppercase tracking-wider text-[var(--color-text-muted)]">{t('sysInfoWifi')}</th>
                    <th class="py-3 px-4 text-[0.75rem] font-semibold uppercase tracking-wider text-[var(--color-text-muted)]">{t('fileColActions')}</th>
                  </tr>
                </thead>
                <tbody>
                  <For each={filtered()}>
                    {(server) => (
                      <tr class="border-b border-[var(--color-border-subtle)] last:border-b-0 hover:bg-[var(--color-bg-surface)]/20 transition">
                        {/* Name + desc */}
                        <td class="py-3 px-4">
                          <div class="flex items-center gap-2">
                            <span class={`w-2 h-2 rounded-full ${server.enabled ? 'bg-[var(--color-accent-soft)]' : 'bg-gray-500'}`} />
                            <div>
                              <div class="font-medium text-[var(--color-text-primary)]">{server.name}</div>
                              <Show when={server.description}>
                                <div class="text-[0.72rem] text-[var(--color-text-muted)] truncate max-w-[200px]">{server.description}</div>
                              </Show>
                            </div>
                          </div>
                        </td>
                        {/* URL */}
                        <td class="py-3 px-4">
                          <code class="text-[0.75rem] text-[var(--color-text-secondary)] break-all max-w-[220px] inline-block">{server.url}</code>
                        </td>
                        {/* Type */}
                        <td class="py-3 px-4">
                          <span class="text-[0.75rem] text-[var(--color-text-secondary)]">
                            {detectType(server.endpoint) === 'sse' ? t('mcpTypeSse') : t('mcpTypeHttp')}
                          </span>
                        </td>
                        {/* Status */}
                        <td class="py-3 px-4">
                          <StatusBadge name={server.name} enabled={server.enabled} />
                        </td>
                        {/* Actions */}
                        <td class="py-3 px-4">
                          <div class="flex items-center gap-1.5 flex-wrap">
                            <Button variant="ghost" size="xs" onClick={() => handleCopyUrl(server.url)}
                              title="Copy URL">
                              {t('mcpCopyUrl')}
                            </Button>
                            <Button variant="ghost" size="xs" onClick={() => openEdit(server)}
                              title="Edit">
                              {t('mcpEdit')}
                            </Button>
                            <Button variant="ghost" size="xs" onClick={() => handleTest(server.name)}
                              disabled={testing()[server.name] === 'checking'}
                              title="Test connection">
                              {t('mcpTestConn')}
                            </Button>
                            <Button variant="ghost" size="xs" onClick={() => handleToggle(server.name)}
                              title={server.enabled ? 'Disable' : 'Enable'}>
                              {server.enabled ? t('mcpToggleDisable') : t('mcpToggleEnable')}
                            </Button>
                            <Button variant="danger-ghost" size="xs" onClick={() => handleRemove(server.name)}
                              title="Delete">
                              {t('mcpRemoveBtn')}
                            </Button>
                          </div>
                        </td>
                      </tr>
                    )}
                  </For>
                </tbody>
              </table>
            </div>

            {/* Footer summary */}
            <div class="flex items-center justify-between text-[0.75rem] text-[var(--color-text-muted)]">
              <span>{servers().length} server(s)</span>
            </div>
          </Show>
        </Show>
      </div>

      {/* Reload hint */}
      <div class="px-5 pb-4">
        <p class="text-[0.72rem] text-[var(--color-text-muted)] m-0">{t('mcpReloadHint')}</p>
      </div>

      {/* Modal */}
      <Show when={showModal()}>
        <ModalForm />
      </Show>
    </TabShell>
  );
};
