/* Warcraft Survivors launcher (build-engine.py --survivor). Loaded before
 * installer.js and filesystem-bridge.js:
 * - the game data is self-hosted beside the page (data/files.json, written by
 *   port/web/make_survivor_data.py): the first visit downloads it into OPFS
 *   with install-worker.js, later visits start at once until its id changes;
 *   without data/files.json the page falls back to importing local files;
 * - the engine starts on the survivor menu map like play_survivor.bat. */
(function () {
  'use strict';
  const DATA = 'data/';
  // Survivors needs The Frozen Throne archives (filesystem-bridge.js mode).
  sessionStorage.setItem('openrealm-wc3-browser-mode', 'tft');
  window.openrealmExtraArguments = [
    '+set', 'wc3_loading_quiet', '1',
    '+set', 'scr_showfps', '0',
    '+set', 'wc3_lua_script', 'Scripts/Survivor/ashenvale_survivor.lua,Scripts/Survivor/engine_boot.lua',
    '+map', 'Maps/FrozenThrone/(6)Moonglade.w3x'
  ];

  const $ = id => document.getElementById(id);
  function status(message) {
    const node = $('status');
    if (node) node.textContent = message;
  }
  const mib = bytes => (bytes / (1024 * 1024)).toFixed(0) + ' MB';
  function hide(node) {
    if (node) node.hidden = true;
  }

  // While playing, the game fills the window (like the iOS web-app layout);
  // only the fullscreen button stays: Esc is a key and the RTS shortcut
  // overlays (Game, 1-9) mean nothing here.
  const style = document.createElement('style');
  style.textContent = [
    'body.survivor-playing { overflow: hidden; }',
    'body.survivor-playing #game-container:not([hidden]) { position: fixed; inset: 0; z-index: 10;',
    '  width: 100vw; height: 100dvh; max-width: none; background: #000; }',
    'body.survivor-playing #game-container:not([hidden]) > canvas { width: 100% !important;',
    '  height: 100% !important; min-height: 0; aspect-ratio: auto !important; }',
    'body.survivor-playing #game-controls > :not(#game-fullscreen) { display: none; }',
    'body.survivor-playing #game-controls { opacity: .35; }',
    'body.survivor-playing #game-controls:hover { opacity: 1; }'
  ].join('\n');
  document.head.appendChild(style);
  const present = window.openrealmGamePresentation;
  window.openrealmGamePresentation = function (state) {
    if (present) present(state);
    document.body.classList.toggle('survivor-playing', state === 'playing');
  };

  document.addEventListener('DOMContentLoaded', () => {
    // The edition is fixed (TFT): drop the selector's section.
    const mode = $('mode-heading');
    hide(mode && mode.closest('section'));
  });

  async function localManifest() {
    try {
      let dir = await navigator.storage.getDirectory();
      for (const name of ['openrealm', 'game-data', 'wc3'])
        dir = await dir.getDirectoryHandle(name);
      const file = await (await dir.getFileHandle('installation.json')).getFile();
      return JSON.parse(await file.text());
    } catch (_) {
      return null;
    }
  }

  function download(hosted) {
    let bar = $('survivor-progress');
    if (!bar) {
      bar = document.createElement('progress');
      bar.id = 'survivor-progress';
      bar.max = 1;
      bar.value = 0;
      $('status').after(bar);
    }
    return new Promise((resolve, reject) => {
      const worker = new Worker('install-worker.js');
      worker.onmessage = ({data}) => {
        if (data.type === 'progress') {
          bar.value = data.total ? data.copied / data.total : 0;
          status('Downloading game data (first visit only): ' + mib(data.copied) + ' / ' + mib(data.total) +
            ' (' + Math.round(100 * bar.value) + '%)');
        } else if (data.type === 'phase' && data.phase === 'storage') {
          status('Checking browser storage...');
        } else if (data.type === 'done') {
          worker.terminate();
          bar.remove();
          resolve();
        } else if (data.type === 'error') {
          worker.terminate();
          reject(new Error('Game data download failed: ' + data.message));
        }
      };
      worker.onerror = () => reject(new Error('Game data worker failed'));
      worker.postMessage({type:'download', hosted});
    });
  }

  window.openrealmBeforeStart = async function () {
    let hosted = null;
    try {
      const response = await fetch(DATA + 'files.json', {cache:'no-store'});
      if (response.ok) hosted = await response.json();
    } catch (_) {
      hosted = null;
    }
    if (!hosted) return; // No hosted data: the import panel stays.
    hide($('data-panel'));
    hosted.base = DATA;
    // About 1.2 GB: ask the browser not to evict it under storage pressure.
    if (navigator.storage && navigator.storage.persist)
      await navigator.storage.persist().catch(() => false);
    const local = await localManifest();
    if (local && local.hosted === hosted.id) return;
    status('Preparing game data download...');
    await download(hosted);
    status('Game data ready; starting...');
  };
})();
