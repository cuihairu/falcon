// Cross-browser API shim: Firefox exposes WebExtension APIs as `browser`
// (promise-based), Chromium as `chrome` (callback-based; MV3 supports await
// on most of them). Every call site in this extension goes through `api`.
// Declared with `var` so the binding lands on the global object and stays
// visible across scripts evaluated in sequence: the popup/options/batch
// pages (classic script here, module consumers later), the Firefox
// background script list, and after importScripts() in the Chromium
// service worker.
var api = typeof browser !== "undefined" ? browser : chrome;
