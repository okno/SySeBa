(() => {
  'use strict';

  const language = document.body.dataset.language === 'en' ? 'en' : 'it';
  const translations = {
    it: {
      loading_status: 'Caricamento stato...', web_token: 'Token web SySeBa', sign_in: 'Accedi', forget_token: 'Dimentica token',
      status: 'Stato', logs: 'Log', configuration: 'Configurazione', restore: 'Restore', overview: 'Panoramica',
      restart_required: 'Riavvio necessario', restart_explanation: 'La configurazione salvata non e ancora attiva.', restart_service: 'Riavvia servizio',
      session_activity: 'Attivita dalla partenza', rows: 'Righe', search: 'Cerca', search_logs: 'Testo nel log', level: 'Livello', all: 'Tutti',
      auto_refresh: 'Aggiorna automaticamente', refresh: 'Aggiorna', copy: 'Copia', download: 'Download', follow_tail: 'Segui ultima riga',
      source: 'Source', backup: 'Backup', log_file: 'File di log', workers: 'Thread worker', save_configuration: 'Salva configurazione', reload: 'Ricarica',
      restore_area: 'Area restore', up: 'Su', search_folder: 'Cerca nella cartella', sort_by: 'Ordina per', name: 'Nome', modified: 'Modifica',
      size: 'Dimensione', direction: 'Direzione', ascending: 'Crescente', descending: 'Decrescente', per_page: 'Per pagina', apply: 'Applica',
      type: 'Tipo', actions: 'Azioni', previous: 'Precedente', next: 'Successiva', confirm_restore: 'Conferma ripristino', cancel: 'Annulla',
      restore_rename: 'Ripristina con nuovo nome', restore_overwrite: 'Sovrascrivi o unisci', restore_now: 'Ripristina',
      auth_required: 'Inserisci il token per consultare e amministrare SySeBa.', auth_invalid: 'Token non valido o mancante.', signed_in: 'Accesso effettuato.',
      token_forgotten: 'Token dimenticato.', authentication_required: 'Autenticazione richiesta', request_failed: 'Operazione non riuscita.',
      request_timeout: 'Il server non ha risposto entro il tempo previsto.', offline: 'Dashboard non raggiungibile', running: 'ATTIVO', web_only: 'SOLO WEB',
      pid: 'PID', cpu: 'CPU', ram: 'RAM', initial_sync: 'Sync iniziale', queue: 'Coda', process_threads: 'thread processo',
      sync_pending: 'in attesa', sync_running: 'in corso', sync_completed: 'completata', sync_completed_with_errors: 'completata con errori',
      sync_skipped: 'saltata', sync_stopped: 'interrotta', sync_failed: 'fallita', sync_not_available: 'non disponibile', not_available: 'n/d',
      used: 'usato', free: 'liberi', path_not_found: 'Percorso non trovato', copied: 'Copiati', updated: 'Aggiornati',
      deleted: 'Cancellati in restore', restored: 'Ripristinati', skipped: 'Saltati', errors: 'Errori', events_received: 'Eventi ricevuti',
      last_update: 'Ultimo aggiornamento', session_started: 'Sessione avviata', restart_manual: 'Riavvio manuale richiesto',
      config_no_changes: 'La configurazione salvata coincide con quella attiva.', pending_changes: 'Modifiche salvate in attesa di riavvio',
      field: 'Campo', active_value: 'Valore attivo', saved_value: 'Valore salvato', configuration_saved: 'Configurazione salvata.',
      discard_changes: 'Scartare le modifiche non salvate?', no_logs: 'Nessun log disponibile.', visible_lines: 'righe visualizzate',
      logs_copied: 'Log copiato negli appunti.', copy_failed: 'Impossibile copiare il log.', directory: 'cartella', file: 'file',
      open: 'Apri', conflict: 'Esiste gia nella source', empty_restore: 'Nessun elemento in questa cartella.', items: 'elementi',
      page: 'Pagina', of: 'di', restore_question: 'Ripristinare questo elemento nella source?',
      restore_conflict_question: 'La destinazione esiste gia. Scegli come procedere.', destination: 'Destinazione',
      restore_success: 'Ripristino completato', download_failed: 'Download non riuscito.',
      service_restart_question: 'Riavviare ora il servizio SySeBa?', service_restarting: 'Riavvio del servizio richiesto.'
    },
    en: {
      loading_status: 'Loading status...', web_token: 'SySeBa web token', sign_in: 'Sign in', forget_token: 'Forget token',
      status: 'Status', logs: 'Logs', configuration: 'Configuration', restore: 'Restore', overview: 'Overview',
      restart_required: 'Restart required', restart_explanation: 'The saved configuration is not active yet.', restart_service: 'Restart service',
      session_activity: 'Activity since startup', rows: 'Lines', search: 'Search', search_logs: 'Text in log', level: 'Level', all: 'All',
      auto_refresh: 'Refresh automatically', refresh: 'Refresh', copy: 'Copy', download: 'Download', follow_tail: 'Follow last line',
      source: 'Source', backup: 'Backup', log_file: 'Log file', workers: 'Worker threads', save_configuration: 'Save configuration', reload: 'Reload',
      restore_area: 'Restore area', up: 'Up', search_folder: 'Search this folder', sort_by: 'Sort by', name: 'Name', modified: 'Modified',
      size: 'Size', direction: 'Direction', ascending: 'Ascending', descending: 'Descending', per_page: 'Per page', apply: 'Apply',
      type: 'Type', actions: 'Actions', previous: 'Previous', next: 'Next', confirm_restore: 'Confirm restore', cancel: 'Cancel',
      restore_rename: 'Restore with a new name', restore_overwrite: 'Overwrite or merge', restore_now: 'Restore',
      auth_required: 'Enter the token to view and manage SySeBa.', auth_invalid: 'Invalid or missing token.', signed_in: 'Signed in.',
      token_forgotten: 'Token forgotten.', authentication_required: 'Authentication required', request_failed: 'Operation failed.',
      request_timeout: 'The server did not respond in time.', offline: 'Dashboard unavailable', running: 'RUNNING', web_only: 'WEB ONLY',
      pid: 'PID', cpu: 'CPU', ram: 'RAM', initial_sync: 'Initial sync', queue: 'Queue', process_threads: 'process threads',
      sync_pending: 'pending', sync_running: 'running', sync_completed: 'completed', sync_completed_with_errors: 'completed with errors',
      sync_skipped: 'skipped', sync_stopped: 'stopped', sync_failed: 'failed', sync_not_available: 'not available', not_available: 'n/a',
      used: 'used', free: 'free', path_not_found: 'Path not found', copied: 'Copied', updated: 'Updated',
      deleted: 'Deleted to restore', restored: 'Restored', skipped: 'Skipped', errors: 'Errors', events_received: 'Events received',
      last_update: 'Last update', session_started: 'Session started', restart_manual: 'Manual restart required',
      config_no_changes: 'The saved configuration matches the active configuration.', pending_changes: 'Saved changes waiting for restart',
      field: 'Field', active_value: 'Active value', saved_value: 'Saved value', configuration_saved: 'Configuration saved.',
      discard_changes: 'Discard unsaved changes?', no_logs: 'No logs available.', visible_lines: 'visible lines',
      logs_copied: 'Logs copied to clipboard.', copy_failed: 'Unable to copy logs.', directory: 'directory', file: 'file',
      open: 'Open', conflict: 'Already exists in source', empty_restore: 'No items in this folder.', items: 'items',
      page: 'Page', of: 'of', restore_question: 'Restore this item to the source?',
      restore_conflict_question: 'The destination already exists. Choose how to continue.', destination: 'Destination',
      restore_success: 'Restore completed', download_failed: 'Download failed.',
      service_restart_question: 'Restart the SySeBa service now?', service_restarting: 'Service restart requested.'
    }
  };
  const text = translations[language];
  const tabs = Array.from(document.querySelectorAll('.tabs [role="tab"]'));
  const tabPanels = Array.from(document.querySelectorAll('[role="tabpanel"]'));
  const restoreState = {path: '', page: 1, pages: 1, search: '', sort: 'name', direction: 'asc', pageSize: 100};
  let currentTab = null;
  let authToken = sessionStorage.getItem('sysebaAuthToken') || '';
  let pendingAuthAction = null;
  let statusLoading = false;
  let logsLoading = false;
  let rawLogLines = [];
  let filteredLogLines = [];
  let configBaseline = '';
  let selectedRestore = null;
  let lastConnectionError = '';

  class AuthRequiredError extends Error {}

  function t(key) {
    return text[key] || key;
  }

  function applyTranslations() {
    document.querySelectorAll('[data-i18n]').forEach((element) => {
      element.textContent = t(element.dataset.i18n);
    });
    document.querySelectorAll('[data-i18n-placeholder]').forEach((element) => {
      element.placeholder = t(element.dataset.i18nPlaceholder);
    });
  }

  function escapeHtml(value) {
    return String(value ?? '').replace(/[&<>"']/g, (character) => ({
      '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;'
    })[character]);
  }

  function authHeaders() {
    return authToken ? {'X-SySeBa-Token': authToken} : {};
  }

  function showAuth(message) {
    const panel = document.getElementById('auth-panel');
    panel.classList.remove('hide');
    document.getElementById('auth-notice').textContent = message || t('auth_required');
    document.getElementById('auth-token').value = authToken;
    document.getElementById('auth-clear').classList.toggle('hide', !authToken);
  }

  function hideAuth() {
    document.getElementById('auth-panel').classList.add('hide');
    document.getElementById('auth-notice').textContent = '';
  }

  function showToast(message, tone) {
    if (!message) return;
    const region = document.getElementById('toast-region');
    const toast = document.createElement('div');
    toast.className = ('toast ' + (tone || '')).trim();
    toast.textContent = message;
    region.replaceChildren(toast);
    window.setTimeout(() => toast.remove(), tone === 'error' ? 6000 : 3200);
  }

  async function request(path, options) {
    const requestOptions = options || {};
    const controller = new AbortController();
    const timeout = window.setTimeout(() => controller.abort(), 15000);
    const headers = Object.assign({}, authHeaders(), requestOptions.headers || {});
    if (requestOptions.body) headers['Content-Type'] = 'application/json';
    let response;
    try {
      response = await fetch(path, Object.assign({}, requestOptions, {headers, signal: controller.signal}));
    } catch (error) {
      if (error.name === 'AbortError') throw new Error(t('request_timeout'));
      throw new Error(t('offline'));
    } finally {
      window.clearTimeout(timeout);
    }
    if (response.status === 401) throw new AuthRequiredError(t('authentication_required'));
    let data = {};
    try {
      data = await response.json();
    } catch (error) {
      throw new Error(t('request_failed'));
    }
    if (!response.ok || data.ok === false) {
      const failure = new Error(data.error || t('request_failed'));
      failure.code = data.code || 'request_failed';
      throw failure;
    }
    hideAuth();
    return data;
  }

  async function runAction(action, options) {
    const settings = options || {};
    const button = settings.button || null;
    if (button) button.disabled = true;
    try {
      return await action();
    } catch (error) {
      if (error instanceof AuthRequiredError) {
        pendingAuthAction = () => runAction(action, settings);
        showAuth(t('auth_invalid'));
      } else if (!settings.silent) {
        showToast(error.message || t('request_failed'), 'error');
      }
      return null;
    } finally {
      if (button) button.disabled = false;
    }
  }

  function formatDate(value) {
    if (!value) return t('not_available');
    const date = new Date(value);
    if (Number.isNaN(date.getTime())) return String(value).replace('T', ' ');
    return new Intl.DateTimeFormat(language, {dateStyle: 'short', timeStyle: 'medium'}).format(date);
  }

  function formatBytes(value) {
    let size = Number(value || 0);
    const units = ['B', 'KB', 'MB', 'GB', 'TB', 'PB'];
    let index = 0;
    while (size >= 1024 && index < units.length - 1) {
      size /= 1024;
      index += 1;
    }
    return size.toFixed(1) + ' ' + units[index];
  }

  function pct(value) {
    if (value === null || value === undefined) return t('not_available');
    return Number(value).toFixed(2) + '%';
  }

  function metric(label, value, detail) {
    return '<div class="panel"><div class="metric-label">' + escapeHtml(label) +
      '</div><div class="metric-value">' + escapeHtml(value) +
      '</div><div class="metric-small">' + escapeHtml(detail || '') + '</div></div>';
  }

  function diskPanel(label, item) {
    const value = item.exists ? Number(item.used_percent || 0) : 0;
    const tone = value >= 90 ? 'danger' : (value >= 75 ? 'warn' : '');
    const detail = item.exists ? pct(value) + ' ' + t('used') + ' - ' + formatBytes(item.free) + ' ' + t('free') : t('path_not_found');
    return '<div class="panel"><h2>' + escapeHtml(label) + '</h2><div class="path">' +
      escapeHtml(item.path) + '</div><div class="bar ' + tone +
      '" role="progressbar" aria-valuemin="0" aria-valuemax="100" aria-valuenow="' + value +
      '"><span style="width:' + Math.max(0, Math.min(value, 100)) + '%"></span></div><div class="metric-small">' +
      escapeHtml(detail) + '</div></div>';
  }

  function syncLabel(state) {
    return t('sync_' + (state || 'pending'));
  }

  function renderStatus(status) {
    const runtimeClass = status.running ? 'status-ok' : 'status-neutral';
    const runtimeText = status.running ? t('running') : t('web_only');
    document.getElementById('runtime-pill').innerHTML = '<span class="' + runtimeClass + '">' +
      escapeHtml(runtimeText) + '</span> ' + escapeHtml(status.uptime);
    document.getElementById('last-updated').textContent = t('last_update') + ': ' + formatDate(status.now);
    document.getElementById('session-started').textContent = t('session_started') + ': ' + formatDate(status.started_at);

    const sync = status.initial_sync || {state: 'pending', percent: status.initial_sync_percent};
    const syncDetails = [syncLabel(sync.state)];
    if (sync.total) syncDetails.push(String(sync.done) + '/' + String(sync.total));
    document.getElementById('metrics').innerHTML = [
      metric(t('pid'), status.pid, status.config.config_path),
      metric(t('cpu'), pct(status.process.cpu_percent), String(status.process.threads) + ' ' + t('process_threads')),
      metric(t('ram'), status.process.memory_mb === null ? t('not_available') : status.process.memory_mb + ' MB',
        t('queue') + ': ' + status.process.queue_size),
      metric(t('initial_sync'), pct(sync.percent), syncDetails.join(' - '))
    ].join('');
    document.getElementById('disk').innerHTML = [
      diskPanel(t('source'), status.disk.source),
      diskPanel(t('backup'), status.disk.backup),
      diskPanel(t('restore'), status.disk.restore)
    ].join('');

    const stats = status.stats;
    const entries = [
      [t('copied'), stats.copied], [t('updated'), stats.updated], [t('deleted'), stats.deleted], [t('restored'), stats.restored],
      [t('skipped'), stats.skipped], [t('errors'), stats.errors], [t('events_received'), stats.queued_events], [t('queue'), status.process.queue_size]
    ];
    document.getElementById('activity').innerHTML = entries.map((entry) =>
      '<div><dt>' + escapeHtml(entry[0]) + '</dt><dd>' + escapeHtml(entry[1]) + '</dd></div>'
    ).join('');

    const banner = document.getElementById('restart-banner');
    banner.classList.toggle('hide', !status.restart_required);
    if (status.restart_required) {
      const service = status.service || {};
      document.getElementById('restart-command').textContent = service.restart_available ? '' :
        t('restart_manual') + ': ' + (service.restart_command || 'sudo systemctl restart syseba.service');
      document.getElementById('restart-service').classList.toggle('hide', !service.restart_available);
    }
  }

  async function loadStatus() {
    if (statusLoading) return false;
    statusLoading = true;
    try {
      const status = await request('/api/status');
      renderStatus(status);
      lastConnectionError = '';
      return true;
    } catch (error) {
      if (error instanceof AuthRequiredError) {
        showAuth(authToken ? t('auth_invalid') : t('auth_required'));
        document.getElementById('runtime-pill').textContent = t('authentication_required');
      } else {
        document.getElementById('runtime-pill').innerHTML = '<span class="status-bad">' + escapeHtml(t('offline')) + '</span>';
        if (lastConnectionError !== error.message) showToast(error.message, 'error');
        lastConnectionError = error.message;
      }
      return false;
    } finally {
      statusLoading = false;
    }
  }

  function activateTab(name, focus) {
    if (name === currentTab) {
      if (focus) document.getElementById('tab-button-' + name).focus();
      return true;
    }
    if (currentTab === 'config' && configIsDirty() && !window.confirm(t('discard_changes'))) return false;
    currentTab = name;
    tabs.forEach((button) => {
      const active = button.dataset.tab === name;
      button.classList.toggle('active', active);
      button.setAttribute('aria-selected', String(active));
      button.tabIndex = active ? 0 : -1;
      if (active && focus) button.focus();
    });
    tabPanels.forEach((panel) => {
      const active = panel.id === 'tab-' + name;
      panel.classList.toggle('hide', !active);
      panel.hidden = !active;
    });
    if (name === 'logs') loadLogs();
    if (name === 'config') loadConfig();
    if (name === 'restore') loadRestore(restoreState.path, restoreState.page);
    return true;
  }

  function renderLogs() {
    const query = document.getElementById('log-search').value.trim().toLowerCase();
    const requestedLevel = document.getElementById('log-level').value;
    filteredLogLines = rawLogLines.filter((line) => {
      const match = line.match(/\[(INFO|WARNING|ERROR|CRITICAL|DEBUG)\]/i);
      const level = match ? match[1].toLowerCase() : 'info';
      const levelMatches = requestedLevel === 'all' || requestedLevel === level ||
        (requestedLevel === 'error' && level === 'critical');
      return levelMatches && (!query || line.toLowerCase().includes(query));
    });
    const container = document.getElementById('logs');
    container.replaceChildren();
    if (!filteredLogLines.length) {
      const empty = document.createElement('div');
      empty.className = 'empty-state';
      empty.textContent = t('no_logs');
      container.appendChild(empty);
    } else {
      const fragment = document.createDocumentFragment();
      filteredLogLines.forEach((line) => {
        const row = document.createElement('div');
        const match = line.match(/\[(INFO|WARNING|ERROR|CRITICAL|DEBUG)\]/i);
        row.className = 'log-line ' + (match ? match[1] : 'info').toLowerCase();
        row.textContent = line;
        fragment.appendChild(row);
      });
      container.appendChild(fragment);
    }
    document.getElementById('logs-meta').textContent = filteredLogLines.length + '/' + rawLogLines.length + ' ' + t('visible_lines');
    if (document.getElementById('log-follow').checked) container.scrollTop = container.scrollHeight;
  }

  async function loadLogs(silent) {
    if (logsLoading) return;
    logsLoading = true;
    const container = document.getElementById('logs');
    container.setAttribute('aria-busy', 'true');
    await runAction(async () => {
      const lines = document.getElementById('log-lines').value;
      const data = await request('/api/logs?lines=' + encodeURIComponent(lines));
      rawLogLines = data.lines || [];
      renderLogs();
    }, {silent: Boolean(silent)});
    container.setAttribute('aria-busy', 'false');
    logsLoading = false;
  }

  function serializeConfigForm() {
    return JSON.stringify(Object.fromEntries(new FormData(document.getElementById('config-form')).entries()));
  }

  function configIsDirty() {
    return Boolean(configBaseline && serializeConfigForm() !== configBaseline);
  }

  function renderConfigState(state) {
    const form = document.getElementById('config-form');
    Object.entries(state.saved).forEach((entry) => {
      const input = form.elements.namedItem(entry[0]);
      if (input) input.value = entry[1];
    });
    configBaseline = serializeConfigForm();
    const diff = document.getElementById('config-diff');
    const changes = Object.entries(state.changes || {});
    if (!changes.length) {
      diff.innerHTML = '<div class="inline-banner"><div><strong>' + escapeHtml(t('config_no_changes')) + '</strong></div></div>';
      return;
    }
    const rows = changes.map((entry) => {
      const values = entry[1];
      return '<tr><th scope="row">' + escapeHtml(entry[0]) + '</th><td class="path">' +
        escapeHtml(values.active) + '</td><td class="changed-value">' + escapeHtml(values.saved) + '</td></tr>';
    }).join('');
    diff.innerHTML = '<div class="inline-banner"><div><strong>' + escapeHtml(t('pending_changes')) +
      '</strong></div></div><table><thead><tr><th scope="col">' + escapeHtml(t('field')) +
      '</th><th scope="col">' + escapeHtml(t('active_value')) + '</th><th scope="col">' +
      escapeHtml(t('saved_value')) + '</th></tr></thead><tbody>' + rows + '</tbody></table>';
  }

  async function loadConfig() {
    return runAction(async () => {
      const state = await request('/api/config/state');
      renderConfigState(state);
      document.getElementById('config-notice').textContent = '';
    });
  }

  async function saveConfig(event) {
    event.preventDefault();
    const form = document.getElementById('config-form');
    if (!form.reportValidity()) return;
    const button = document.getElementById('save-config');
    await runAction(async () => {
      const data = Object.fromEntries(new FormData(form).entries());
      const result = await request('/api/config', {method: 'POST', body: JSON.stringify(data)});
      renderConfigState(result.state);
      document.getElementById('config-notice').textContent = result.message || t('configuration_saved');
      showToast(result.message || t('configuration_saved'), 'success');
      await loadStatus();
    }, {button});
  }

  function createButton(label, action, path, primary) {
    const button = document.createElement('button');
    button.type = 'button';
    button.textContent = label;
    button.dataset.action = action;
    button.dataset.path = path;
    if (primary) button.className = 'primary';
    return button;
  }

  function renderBreadcrumbs(path) {
    const container = document.getElementById('restore-breadcrumbs');
    container.replaceChildren();
    container.appendChild(createButton('/', 'open', ''));
    const parts = path.split('/').filter(Boolean);
    let accumulated = '';
    parts.forEach((part) => {
      accumulated = accumulated ? accumulated + '/' + part : part;
      const separator = document.createElement('span');
      separator.textContent = '/';
      container.appendChild(separator);
      container.appendChild(createButton(part, 'open', accumulated));
    });
  }

  function createRestoreCell(value, label, className) {
    const cell = document.createElement('td');
    cell.dataset.label = label;
    cell.className = className || '';
    cell.textContent = value;
    return cell;
  }

  function renderRestore(data) {
    restoreState.path = data.path || '';
    restoreState.page = data.page || 1;
    restoreState.pages = data.pages || 1;
    renderBreadcrumbs(restoreState.path);
    document.getElementById('restore-up').disabled = !restoreState.path;
    document.getElementById('restore-total').textContent = data.total + ' ' + t('items');
    document.getElementById('restore-page-info').textContent = t('page') + ' ' + data.page + ' ' + t('of') + ' ' + data.pages;
    document.getElementById('restore-prev').disabled = !data.has_previous;
    document.getElementById('restore-next').disabled = !data.has_next;

    const body = document.getElementById('restore-list');
    body.replaceChildren();
    if (!data.items.length) {
      const row = document.createElement('tr');
      const cell = document.createElement('td');
      cell.colSpan = 5;
      cell.className = 'empty-state';
      cell.textContent = t('empty_restore');
      row.appendChild(cell);
      body.appendChild(row);
      return;
    }
    const fragment = document.createDocumentFragment();
    data.items.forEach((item) => {
      const row = document.createElement('tr');
      row.className = 'restore-row';
      const nameCell = createRestoreCell(item.name, t('name'), 'path');
      if (item.destination_exists) {
        const conflict = document.createElement('div');
        conflict.className = 'conflict';
        conflict.textContent = t('conflict');
        nameCell.appendChild(conflict);
      }
      row.appendChild(nameCell);
      row.appendChild(createRestoreCell(item.is_dir ? t('directory') : t('file'), t('type')));
      row.appendChild(createRestoreCell(item.size_human, t('size')));
      row.appendChild(createRestoreCell(formatDate(item.mtime), t('modified')));
      const actions = document.createElement('td');
      actions.className = 'actions-cell';
      actions.dataset.label = t('actions');
      const actionGroup = document.createElement('div');
      actionGroup.className = 'row-actions';
      actionGroup.appendChild(item.is_dir ?
        createButton(t('open'), 'open', item.path) :
        createButton(t('download'), 'download', item.path));
      actionGroup.appendChild(createButton(t('restore_now'), 'restore', item.path, true));
      actions.appendChild(actionGroup);
      row.appendChild(actions);
      fragment.appendChild(row);
    });
    body.appendChild(fragment);
  }

  async function loadRestore(path, page, silent) {
    const requestedPath = path === undefined ? restoreState.path : path;
    const requestedPage = page || 1;
    restoreState.path = requestedPath;
    restoreState.page = requestedPage;
    return runAction(async () => {
      const params = new URLSearchParams({
        path: requestedPath, page: String(requestedPage), page_size: String(restoreState.pageSize),
        search: restoreState.search, sort: restoreState.sort, direction: restoreState.direction
      });
      const data = await request('/api/restore?' + params.toString());
      renderRestore(data);
    }, {silent: Boolean(silent)});
  }

  async function openRestoreDialog(path) {
    await runAction(async () => {
      selectedRestore = await request('/api/restore/info?path=' + encodeURIComponent(path));
      document.getElementById('restore-dialog-message').textContent = selectedRestore.destination_exists ?
        t('restore_conflict_question') : t('restore_question');
      document.getElementById('restore-dialog-details').innerHTML =
        '<div><strong>' + escapeHtml(t('name')) + ':</strong> ' + escapeHtml(selectedRestore.path) +
        '</div><div><strong>' + escapeHtml(t('destination')) + ':</strong> ' +
        escapeHtml(selectedRestore.destination) + '</div>';
      document.getElementById('restore-confirm').classList.toggle('hide', selectedRestore.destination_exists);
      document.getElementById('restore-rename').classList.toggle('hide', !selectedRestore.destination_exists);
      document.getElementById('restore-overwrite').classList.toggle('hide', !selectedRestore.destination_exists);
      document.getElementById('restore-dialog').showModal();
    });
  }

  async function performRestore(strategy, button) {
    if (!selectedRestore) return;
    await runAction(async () => {
      const result = await request('/api/restore', {
        method: 'POST',
        body: JSON.stringify({path: selectedRestore.path, strategy})
      });
      document.getElementById('restore-dialog').close();
      showToast(t('restore_success') + ': ' + result.restored_to, 'success');
      selectedRestore = null;
      await loadRestore(restoreState.path, restoreState.page);
      await loadStatus();
    }, {button});
  }

  async function downloadItem(path, button) {
    await runAction(async () => {
      const response = await fetch('/restore/download?path=' + encodeURIComponent(path), {headers: authHeaders()});
      if (response.status === 401) throw new AuthRequiredError(t('authentication_required'));
      if (!response.ok) {
        let message = t('download_failed');
        try {
          message = (await response.json()).error || message;
        } catch (error) {}
        throw new Error(message);
      }
      const blob = await response.blob();
      const url = URL.createObjectURL(blob);
      const link = document.createElement('a');
      link.href = url;
      link.download = path.split('/').pop() || 'download';
      document.body.appendChild(link);
      link.click();
      link.remove();
      window.setTimeout(() => URL.revokeObjectURL(url), 1000);
    }, {button});
  }

  tabs.forEach((button, index) => {
    button.addEventListener('click', () => activateTab(button.dataset.tab));
    button.addEventListener('keydown', (event) => {
      let target = null;
      if (event.key === 'ArrowRight') target = tabs[(index + 1) % tabs.length];
      if (event.key === 'ArrowLeft') target = tabs[(index - 1 + tabs.length) % tabs.length];
      if (event.key === 'Home') target = tabs[0];
      if (event.key === 'End') target = tabs[tabs.length - 1];
      if (target) {
        event.preventDefault();
        activateTab(target.dataset.tab, true);
      }
    });
  });

  document.getElementById('auth-form').addEventListener('submit', async (event) => {
    event.preventDefault();
    const button = document.getElementById('auth-save');
    button.disabled = true;
    authToken = document.getElementById('auth-token').value.trim();
    sessionStorage.setItem('sysebaAuthToken', authToken);
    const authenticated = await loadStatus();
    button.disabled = false;
    if (!authenticated) return;
    hideAuth();
    showToast(t('signed_in'), 'success');
    const retry = pendingAuthAction;
    pendingAuthAction = null;
    if (retry) await retry();
    else activateTab(currentTab);
  });

  document.getElementById('auth-clear').addEventListener('click', () => {
    authToken = '';
    sessionStorage.removeItem('sysebaAuthToken');
    showAuth(t('token_forgotten'));
    document.getElementById('auth-token').focus();
  });

  document.getElementById('config-form').addEventListener('submit', saveConfig);
  document.getElementById('reload-config').addEventListener('click', async () => {
    if (configIsDirty() && !window.confirm(t('discard_changes'))) return;
    await loadConfig();
  });

  window.addEventListener('beforeunload', (event) => {
    if (!configIsDirty()) return;
    event.preventDefault();
    event.returnValue = '';
  });

  document.getElementById('refresh-logs').addEventListener('click', () => loadLogs(false));
  document.getElementById('log-lines').addEventListener('change', () => loadLogs());
  document.getElementById('log-search').addEventListener('input', renderLogs);
  document.getElementById('log-level').addEventListener('change', renderLogs);
  document.getElementById('copy-logs').addEventListener('click', async () => {
    try {
      await navigator.clipboard.writeText(filteredLogLines.join('\n'));
      showToast(t('logs_copied'), 'success');
    } catch (error) {
      showToast(t('copy_failed'), 'error');
    }
  });
  document.getElementById('download-logs').addEventListener('click', () => {
    const blob = new Blob([filteredLogLines.join('\n') + '\n'], {type: 'text/plain;charset=utf-8'});
    const url = URL.createObjectURL(blob);
    const link = document.createElement('a');
    link.href = url;
    link.download = 'syseba-' + new Date().toISOString().replace(/[:.]/g, '-') + '.log';
    link.click();
    window.setTimeout(() => URL.revokeObjectURL(url), 1000);
  });

  document.getElementById('restore-filter').addEventListener('submit', (event) => {
    event.preventDefault();
    restoreState.search = document.getElementById('restore-search').value.trim();
    restoreState.sort = document.getElementById('restore-sort').value;
    restoreState.direction = document.getElementById('restore-direction').value;
    restoreState.pageSize = Number(document.getElementById('restore-page-size').value);
    loadRestore(restoreState.path, 1);
  });
  document.getElementById('restore-list').addEventListener('click', (event) => {
    const button = event.target.closest('button[data-action]');
    if (!button) return;
    if (button.dataset.action === 'open') loadRestore(button.dataset.path, 1);
    if (button.dataset.action === 'download') downloadItem(button.dataset.path, button);
    if (button.dataset.action === 'restore') openRestoreDialog(button.dataset.path);
  });
  document.getElementById('restore-breadcrumbs').addEventListener('click', (event) => {
    const button = event.target.closest('button[data-action="open"]');
    if (button) loadRestore(button.dataset.path, 1);
  });
  document.getElementById('restore-up').addEventListener('click', () => {
    const parts = restoreState.path.split('/').filter(Boolean);
    parts.pop();
    loadRestore(parts.join('/'), 1);
  });
  document.getElementById('restore-prev').addEventListener('click', () => {
    loadRestore(restoreState.path, Math.max(1, restoreState.page - 1));
  });
  document.getElementById('restore-next').addEventListener('click', () => {
    loadRestore(restoreState.path, Math.min(restoreState.pages, restoreState.page + 1));
  });
  document.getElementById('restore-cancel').addEventListener('click', () => {
    document.getElementById('restore-dialog').close();
  });
  ['restore-confirm', 'restore-rename', 'restore-overwrite'].forEach((id) => {
    document.getElementById(id).addEventListener('click', (event) => {
      performRestore(event.currentTarget.dataset.strategy, event.currentTarget);
    });
  });

  document.getElementById('restart-service').addEventListener('click', async (event) => {
    if (!window.confirm(t('service_restart_question'))) return;
    await runAction(async () => {
      const result = await request('/api/service/restart', {method: 'POST', body: '{}'});
      showToast(result.message || t('service_restarting'), 'success');
      document.getElementById('runtime-pill').textContent = t('service_restarting');
    }, {button: event.currentTarget});
  });

  document.addEventListener('visibilitychange', () => {
    if (document.hidden) return;
    loadStatus();
    if (currentTab === 'logs' && document.getElementById('log-auto').checked) loadLogs(true);
  });

  applyTranslations();
  activateTab('status');
  loadStatus();
  window.setInterval(() => {
    if (document.hidden) return;
    loadStatus();
    if (currentTab === 'logs' && document.getElementById('log-auto').checked) loadLogs(true);
  }, 3000);
})();
