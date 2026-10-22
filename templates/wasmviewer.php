<?php
/**
 * WebAssembly viewer page.
 *
 * Renders the same ?file= URL the Files app already hands to the PDF.js route,
 * so nothing upstream has to change: open /apps/files_pdfviewer/wasm?file=...
 * and the C++ reader parses the file in this tab.
 *
 * @var array $_ 
 * @var OCP\IURLGenerator $urlGenerator
 */

$urlGenerator = $_['urlGenerator'];
$version = \OCP\App::getAppVersion('files_pdfviewer');
$asset = static function (string $path) use ($urlGenerator, $version): string {
	return $urlGenerator->linkTo('files_pdfviewer', $path) . '?v=' . $version;
};
?>
<!DOCTYPE html>
<html lang="en">
<head>
	<meta charset="utf-8">
	<meta name="viewport" content="width=device-width, initial-scale=1">
	<title>PDF</title>
	<link rel="stylesheet" href="<?php p($asset('js/vendor/wasm-pdf-reader/styles.css')); ?>">
	<link rel="stylesheet" href="<?php p($asset('css/wasmviewer.css')); ?>">
	<script defer src="<?php p($asset('js/wasmviewer.js')); ?>"
			data-bundle="<?php p($asset('js/vendor/wasm-pdf-reader/index.js')); ?>"></script>
</head>
<body>
	<div id="wpdf-root" class="wpdf-host"></div>
	<noscript>This viewer needs JavaScript and WebAssembly.</noscript>
</body>
</html>
