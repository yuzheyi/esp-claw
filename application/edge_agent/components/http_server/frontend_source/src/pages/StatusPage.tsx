import { createSignal, type Component } from 'solid-js';
import { t } from '../i18n';
import { TabShell } from '../components/layout/TabShell';
import { PageHeader } from '../components/ui/PageHeader';
import { StaticConfigBlock } from '../components/ui/ConfigBlocks';
import { Button } from '../components/ui/Button';
import { RestartConfirmModal } from '../components/system/RestartConfirmModal';
import { appStatus, reloadStatus } from '../state/config';
import { pushToast } from '../state/toast';

const InfoRow: Component<{ label: string; value?: string; mono?: boolean }> = (props) => {
  const none = t('sysInfoNone') as string;
  return (
    <div class="flex items-center justify-between py-2 px-3 rounded-[var(--radius-sm)] bg-white/[0.02] border border-transparent hover:border-[var(--color-border-subtle)] gap-3">
      <span class="text-[0.78rem] uppercase tracking-wider text-[var(--color-text-muted)] font-semibold">
        {props.label}
      </span>
      <span
        class={[
          'text-[0.88rem] text-[var(--color-text-primary)] text-right break-all',
          props.mono ? 'font-mono' : '',
        ].join(' ')}
      >
        {props.value || none}
      </span>
    </div>
  );
};

/** Format bytes to human-readable string (e.g. "14.2 GB") */
function formatBytes(bytes: number): string {
  if (bytes <= 0) return '0 B';
  const units = ['B', 'KB', 'MB', 'GB', 'TB'];
  const i = Math.min(Math.floor(Math.log(bytes) / Math.log(1024)), units.length - 1);
  const val = bytes / Math.pow(1024, i);
  return `${val.toFixed(i === 0 ? 0 : 1)} ${units[i]}`;
}

const SdCardStatus: Component = () => {
  const status = appStatus;
  const mounted = () => status()?.sdcard_mounted ?? false;
  const totalBytes = () => status()?.sdcard_total_bytes ?? 0;
  const freeBytes = () => status()?.sdcard_free_bytes ?? 0;
  const usedBytes = () => totalBytes() - freeBytes();
  const usagePercent = () => totalBytes() > 0 ? Math.round((usedBytes() / totalBytes()) * 100) : 0;
  const mountPoint = () => status()?.sdcard_mount_point ?? '';
  const error = () => status()?.sdcard_error ?? '';

  return (
    <div class="pt-2 space-y-2">
      {/* Mount status indicator */}
      <div class="flex items-center justify-between py-2 px-3 rounded-[var(--radius-sm)] bg-white/[0.02] border border-transparent hover:border-[var(--color-border-subtle)] gap-3">
        <span class="text-[0.78rem] uppercase tracking-wider text-[var(--color-text-muted)] font-semibold">
          {t('sysInfoSdCardStatus') as string}
        </span>
        <span class="flex items-center gap-2 text-[0.88rem]">
          <span
            class={`inline-block w-2 h-2 rounded-full ${mounted() ? 'bg-green-400' : 'bg-red-400'}`}
          />
          <span class={mounted() ? 'text-green-400' : 'text-red-400'}>
            {mounted() ? (t('sysInfoSdCardMounted') as string) : (t('sysInfoSdCardNotMounted') as string)}
          </span>
        </span>
      </div>

      {/* Capacity info — only shown when mounted */}
      {mounted() && (
        <>
          <InfoRow label={t('sysInfoSdCardMountPoint') as string} value={mountPoint()} mono />
          <InfoRow label={t('sysInfoSdCardTotal') as string} value={formatBytes(totalBytes())} mono />
          <InfoRow label={t('sysInfoSdCardUsed') as string} value={formatBytes(usedBytes())} mono />
          <InfoRow label={t('sysInfoSdCardFree') as string} value={formatBytes(freeBytes())} mono />

          {/* Usage bar */}
          <div class="px-3 py-2">
            <div class="flex items-center justify-between mb-1">
              <span class="text-[0.72rem] uppercase tracking-wider text-[var(--color-text-muted)] font-semibold">
                {t('sysInfoSdCardUsage') as string}
              </span>
              <span class="text-[0.78rem] text-[var(--color-text-secondary)] font-mono">
                {usagePercent()}%
              </span>
            </div>
            <div class="w-full h-2 rounded-full bg-white/5 overflow-hidden">
              <div
                class="h-full rounded-full transition-all duration-500"
                style={{
                  width: `${usagePercent()}%`,
                  'background-color':
                    usagePercent() > 90
                      ? 'var(--color-danger, #ef4444)'
                      : usagePercent() > 70
                        ? 'var(--color-warning, #f59e0b)'
                        : 'var(--color-success, #22c55e)',
                }}
              />
            </div>
          </div>
        </>
      )}

      {/* Error message — shown when not mounted */}
      {!mounted() && error() && (
        <div class="px-3 py-2 rounded-[var(--radius-sm)] bg-red-500/5 border border-red-500/20">
          <span class="text-[0.82rem] text-red-400">{error()}</span>
        </div>
      )}
    </div>
  );
};

export const StatusPage: Component<{ onRestartRequest: () => void }> = (props) => {
  const [confirmOpen, setConfirmOpen] = createSignal(false);

  const reload = async () => {
    try {
      await reloadStatus();
      pushToast(t('sysInfoReload') as string, 'success');
    } catch (err) {
      pushToast((err as Error).message, 'error');
    }
  };

  return (
    <TabShell>
      <PageHeader
        title={t('navStatus') as string}
        description={t('pageSubtitle') as string}
        actions={
          <>
            <Button size="sm" variant="secondary" onClick={reload}>
              {t('sysInfoReload')}
            </Button>
            <Button size="sm" variant="secondary" onClick={() => setConfirmOpen(true)}>
              {t('sysInfoRestart')}
            </Button>
          </>
        }
      />
      <div class="divide-y divide-[var(--color-border-subtle)] mt-2">
        <StaticConfigBlock title={t('sectionStatusNetwork') as string}>
          <div class="grid gap-2 sm:grid-cols-2 pt-2">
            <InfoRow
              label={t('sysInfoWifi') as string}
              value={
                appStatus()?.wifi_connected
                  ? (t('statusOnline') as string)
                  : appStatus()?.ap_active
                    ? (t('statusApActive') as string)
                    : (t('statusOffline') as string)
              }
            />
            <InfoRow label={t('sysInfoIp') as string} value={appStatus()?.ip} mono />
            <InfoRow label={t('sysInfoMode') as string} value={appStatus()?.wifi_mode} mono />
            <InfoRow label={t('sysInfoApSsid') as string} value={appStatus()?.ap_ssid} mono />
            <InfoRow label={t('sysInfoApIp') as string} value={appStatus()?.ap_ip} mono />
          </div>
        </StaticConfigBlock>
        <StaticConfigBlock title={t('sectionStatusStorage') as string}>
          <div class="grid gap-2 sm:grid-cols-2 pt-2">
            <InfoRow
              label={t('sysInfoStorage') as string}
              value={appStatus()?.storage_base_path}
              mono
            />
          </div>
        </StaticConfigBlock>
        <StaticConfigBlock title={t('sectionStatusSdCard') as string}>
          <SdCardStatus />
        </StaticConfigBlock>
      </div>
      <RestartConfirmModal
        open={confirmOpen()}
        onClose={() => setConfirmOpen(false)}
        onConfirm={() => {
          setConfirmOpen(false);
          props.onRestartRequest();
        }}
      />
    </TabShell>
  );
};
