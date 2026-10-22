/**
 * Mounts the WebAssembly PDF viewer.
 *
 * The Files app opens this page as ?file=<download url>, exactly as it does for
 * the PDF.js route. The bundle is imported from data-bundle so the template
 * stays free of inline script (the page CSP allows no inline JS).
 */
(function () {
	'use strict';

	var script = document.currentScript;
	var root = document.getElementById('wpdf-root');
	var file = new URLSearchParams(window.location.search).get('file');

	function fail(message) {
		root.className = 'wpdf-host wpdf-host--error';
		root.textContent = message;
	}

	if (!file) {
		fail('No file given.');
		return;
	}
	if (!script || !script.dataset.bundle) {
		fail('Viewer bundle missing.');
		return;
	}

	import(new URL(script.dataset.bundle, window.location.href).href)
		.then(function (mod) {
			mod.mountPdfReader(root, { file: file });
		})
		.catch(function (err) {
			fail('Could not open this PDF — ' + (err && err.message ? err.message : err));
		});
})();
