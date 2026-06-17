import { createSignal, For, onMount, Show, type Component } from 'solid-js';
import { t } from '../i18n';
import { TabShell } from '../components/layout/TabShell';
import { PageHeader } from '../components/ui/PageHeader';
import { Button } from '../components/ui/Button';
import { Banner } from '../components/ui/Banner';
import { pushToast } from '../state/toast';

type McpServer = {
  name: string;
  url: string;
  token: string;
  endpoint: string;
  description: string;
  enabled: boolean;
};

type McpConfig = {
  servers: Record<string, Omit<McpServer, 'name'>>;
};

export const McpPage: Component = () => {
  const [servers, setServers] = createSignal<McpServer[]>([]);
  const [loading, setLoading] = createSignal(true);
  const [error, setError] = createSignal<string | null>(null);
  const [showAddForm, setShowAddForm] = createSignal(false);
  const [newServer, setNewServer] = createSignal<McpServer>({
    name: '',
    url: '',
    token: '',
    endpoint: 'mcp',
    description: '',
    enabled: true,
  });

  const loadServers = async () => {
    setLoading(true);
    setError(null);
    try {
      const resp = await fetch('/api/mcp_servers');
      if (!resp.ok) throw new Error(`HTTP ${resp.status}`);
      const data: McpConfig = await resp.json();
      const list: McpServer[] = [];
      if (data.servers) {
        for (const [name, cfg] of Object.entries(data.servers)) {
          list.push({
            name,
            url: cfg.url || '',
            token: cfg.token || '',
            endpoint: cfg.endpoint || 'mcp',
            description: cfg.description || '',
            enabled: cfg.enabled !== false,
          });
        }
      }
      setServers(list);
    } catch (err) {
      setError((err as Error).message);
    } finally {
      setLoading(false);
    }
  };

  onMount(() => void loadServers());

  const handleAdd = async () => {
    const srv = newServer();
    if (!srv.name || !srv.url) {
      pushToast({ kind: 'error', message: 'Name and URL are required' });
      return;
    }
    try {
      const resp = await fetch('/api/mcp_servers', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({
          action: 'add',
          data: srv,
        }),
      });
      if (!resp.ok) throw new Error(`HTTP ${resp.status}`);
      pushToast({ kind: 'success', message: `Server "${srv.name}" added` });
      setShowAddForm(false);
      setNewServer({ name: '', url: '', token: '', endpoint: 'mcp', description: '', enabled: true });
      void loadServers();
    } catch (err) {
      pushToast({ kind: 'error', message: (err as Error).message });
    }
  };

  const handleRemove = async (name: string) => {
    if (!confirm(`Remove server "${name}"?`)) return;
    try {
      const resp = await fetch('/api/mcp_servers', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ action: 'remove', name }),
      });
      if (!resp.ok) throw new Error(`HTTP ${resp.status}`);
      pushToast({ kind: 'success', message: `Server "${name}" removed` });
      void loadServers();
    } catch (err) {
      pushToast({ kind: 'error', message: (err as Error).message });
    }
  };

  const handleToggle = async (name: string) => {
    try {
      const resp = await fetch('/api/mcp_servers', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ action: 'toggle', name }),
      });
      if (!resp.ok) throw new Error(`HTTP ${resp.status}`);
      void loadServers();
    } catch (err) {
      pushToast({ kind: 'error', message: (err as Error).message });
    }
  };

  return (
    <TabShell>
      <PageHeader title="MCP Servers" description="Manage remote MCP server connections" />
      <Show when={error()}>
        <div class="mb-4">
          <Banner kind="error" message={error() ?? undefined} />
        </div>
      </Show>

      <Show when={!loading()}>
        <div class="mb-4 flex gap-2">
          <Button
            variant={showAddForm() ? 'secondary' : 'primary'}
            onClick={() => setShowAddForm(!showAddForm())}
          >
            {showAddForm() ? 'Cancel' : '+ Add Server'}
          </Button>
          <Button variant="ghost" onClick={() => void loadServers()}>
            Refresh
          </Button>
        </div>

        <Show when={showAddForm()}>
          <div class="mb-6 rounded-lg border border-[var(--color-border)] bg-[var(--color-bg-secondary)] p-4">
            <h3 class="mb-3 text-sm font-semibold">Add New MCP Server</h3>
            <div class="grid grid-cols-1 gap-3 sm:grid-cols-2">
              <label class="block">
                <span class="text-xs text-[var(--color-text-muted)]">Name *</span>
                <input
                  class="mt-1 w-full rounded border border-[var(--color-border)] bg-[var(--color-bg)] px-2 py-1 text-sm"
                  value={newServer().name}
                  onInput={(e) => setNewServer({ ...newServer(), name: e.currentTarget.value })}
                  placeholder="my-server"
                />
              </label>
              <label class="block">
                <span class="text-xs text-[var(--color-text-muted)]">URL *</span>
                <input
                  class="mt-1 w-full rounded border border-[var(--color-border)] bg-[var(--color-bg)] px-2 py-1 text-sm"
                  value={newServer().url}
                  onInput={(e) => setNewServer({ ...newServer(), url: e.currentTarget.value })}
                  placeholder="http://192.168.1.100:8080"
                />
              </label>
              <label class="block">
                <span class="text-xs text-[var(--color-text-muted)]">Token</span>
                <input
                  class="mt-1 w-full rounded border border-[var(--color-border)] bg-[var(--color-bg)] px-2 py-1 text-sm"
                  value={newServer().token}
                  onInput={(e) => setNewServer({ ...newServer(), token: e.currentTarget.value })}
                  placeholder="Bearer token (optional)"
                />
              </label>
              <label class="block">
                <span class="text-xs text-[var(--color-text-muted)]">Endpoint</span>
                <input
                  class="mt-1 w-full rounded border border-[var(--color-border)] bg-[var(--color-bg)] px-2 py-1 text-sm"
                  value={newServer().endpoint}
                  onInput={(e) => setNewServer({ ...newServer(), endpoint: e.currentTarget.value })}
                  placeholder="mcp"
                />
              </label>
              <label class="block sm:col-span-2">
                <span class="text-xs text-[var(--color-text-muted)]">Description</span>
                <input
                  class="mt-1 w-full rounded border border-[var(--color-border)] bg-[var(--color-bg)] px-2 py-1 text-sm"
                  value={newServer().description}
                  onInput={(e) => setNewServer({ ...newServer(), description: e.currentTarget.value })}
                  placeholder="What does this server do?"
                />
              </label>
            </div>
            <div class="mt-3">
              <Button variant="primary" onClick={() => void handleAdd()}>
                Save
              </Button>
            </div>
          </div>
        </Show>

        <Show when={servers().length === 0}>
          <p class="text-sm text-[var(--color-text-muted)]">No MCP servers configured.</p>
        </Show>

        <div class="space-y-3">
          <For each={servers()}>
            {(srv) => (
              <div class="rounded-lg border border-[var(--color-border)] bg-[var(--color-bg-secondary)] p-4">
                <div class="flex items-center justify-between">
                  <div class="flex items-center gap-2">
                    <span
                      class={`inline-block h-2 w-2 rounded-full ${srv.enabled ? 'bg-green-500' : 'bg-gray-400'}`}
                    />
                    <h3 class="text-sm font-semibold">{srv.name}</h3>
                  </div>
                  <div class="flex gap-2">
                    <Button variant="ghost" onClick={() => void handleToggle(srv.name)}>
                      {srv.enabled ? 'Disable' : 'Enable'}
                    </Button>
                    <Button variant="danger" onClick={() => void handleRemove(srv.name)}>
                      Remove
                    </Button>
                  </div>
                </div>
                <dl class="mt-2 grid grid-cols-1 gap-x-4 gap-y-1 text-xs sm:grid-cols-2">
                  <div>
                    <dt class="text-[var(--color-text-muted)]">URL</dt>
                    <dd class="break-all font-mono">{srv.url}</dd>
                  </div>
                  <div>
                    <dt class="text-[var(--color-text-muted)]">Endpoint</dt>
                    <dd class="font-mono">{srv.endpoint || '(default)'}</dd>
                  </div>
                  <Show when={srv.description}>
                    <div>
                      <dt class="text-[var(--color-text-muted)]">Description</dt>
                      <dd>{srv.description}</dd>
                    </div>
                  </Show>
                  <Show when={srv.token}>
                    <div>
                      <dt class="text-[var(--color-text-muted)]">Token</dt>
                      <dd class="font-mono">{'•'.repeat(Math.min(srv.token.length, 20))}</dd>
                    </div>
                  </Show>
                </dl>
              </div>
            )}
          </For>
        </div>
      </Show>

      <Show when={loading()}>
        <p class="p-6 text-sm text-[var(--color-text-muted)]">Loading...</p>
      </Show>
    </TabShell>
  );
};
