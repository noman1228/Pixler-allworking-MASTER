// Run with node tests/unzip/ui.cjs. Uses the local UI verification jsdom.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const {createRequire} = require('node:module');
const root = path.resolve(__dirname, '../..');
const localRequire = createRequire(path.join(root, '.pio/ui-verification/package.json'));
const {JSDOM} = localRequire('jsdom');
const html = fs.readFileSync(path.join(root, 'html/index.html'), 'utf8');
const source = fs.readFileSync(path.join(root, 'html/js/script.js'), 'utf8');
const dom = new JSDOM(html, {url: 'http://controller.local/', runScripts: 'outside-only', pretendToBeVisual: true});
const {window} = dom;
window.eval(fs.readFileSync(path.join(root, 'html/js/jquery-3.7.1.js'), 'utf8'));
const $ = window.jQuery;
let fileListRequests = 0;
const requests = [];
window.RequestListOfFiles = () => fileListRequests++;
window.ProcessReceivedJsonStatusMessage = data => window.ProcessUnzipStatus(data.status.system.unzip, true);
$.ajaxQueue = options => requests.push(options);
window.console = {error() {}};
window.eval('var DocumentIsHidden = false; var StatusRequestInProgress = false; var UnzipStatusState = "idle"; var LastUnzipCompletionKey = "";' +
    'var MonitorTransactionRequestInProgressId = 0; var FailedToCompleteServerTransaction = 0;\n' +
    source.slice(source.indexOf('function RequestStatusUpdate()'), source.indexOf('function RequestListOfFiles()')) +
    source.slice(source.indexOf('function ProcessUnzipStatus('), source.indexOf('function ProcessReceivedJsonStatusMessage(')));
const report = overrides => ({
    state: 'extracting', fileName: 'Carol.xlz', entryName: 'Carol.fseq',
    bytesWritten: 1024, totalBytes: 4096, filesExtracted: 0, jobId: 1, ...overrides
});
const display = (status, connected = true) => window.ProcessUnzipStatus(status, connected);

assert.equal($('#unzipStatusIndicator').closest('.mdiv').length, 0, 'status stays outside navigable tabs');
display(undefined);
assert.equal($('#unzipStatusIndicator').hasClass('hidden'), true, 'older firmware without unzip status remains usable');
display(report({state: 'waiting'}), false);
assert.equal($('#unzipStatusIndicator').hasClass('alert-info'), true);
assert.match($('#unzipStatusDetail').text(), /Waiting for a network connection/);
assert.equal($('#unzipStatusProgress').hasClass('hidden'), true);
display(report({state: 'waiting'}), true);
assert.match($('#unzipStatusDetail').text(), /file transfers finish/);
display(report());
assert.equal($('#unzipStatusIndicator').hasClass('alert-warning'), true);
assert.equal($('#unzipStatusProgress').hasClass('hidden'), false);
assert.match($('#unzipStatusDetail').text(), /Carol.fseq.*25%/);
assert.equal($('#unzipStatusProgressBar').attr('aria-valuenow'), '25');
assert.equal($('#unzipStatusProgressBar').css('width'), '25%');
display(report({bytesWritten: 5000}));
assert.equal($('#unzipStatusProgressBar').attr('aria-valuenow'), '100', 'rounding or malformed oversized totals never overflow the bar');
display(report({bytesWritten: -20, totalBytes: 0}));
assert.equal($('#unzipStatusProgressBar').attr('aria-valuenow'), '0', 'zero-sized entries never render NaN');
const unsafe = '<img src=x onerror=alert(1)>.xlz';
display(report({fileName: unsafe, entryName: unsafe}));
assert.ok($('#unzipStatusTitle').text().includes(unsafe));
assert.equal($('#unzipStatusIndicator img').length, 0, 'archive and entry names are text, never HTML');

display(report({state: 'complete', filesExtracted: 1, bytesWritten: 4096}));
assert.equal($('#unzipStatusIndicator').hasClass('alert-success'), true);
assert.equal($('#unzipStatusDetail').text(), '1 file ready.');
assert.equal($('#unzipStatusProgress').hasClass('hidden'), true);
assert.equal(fileListRequests, 1, 'completion refreshes available sequences');
display(report({state: 'complete', filesExtracted: 1, bytesWritten: 4096}));
assert.equal(fileListRequests, 1, 'persistent completion does not repeatedly fetch the file list');
display(report({state: 'complete', filesExtracted: 1, bytesWritten: 4096, jobId: 2}));
assert.equal(fileListRequests, 2, 'reuploading the same archive refreshes the list for a new job');
display(report({state: 'error', error: 'CRC failed: ' + unsafe}));
assert.equal($('#unzipStatusIndicator').hasClass('alert-danger'), true);
assert.match($('#unzipStatusDetail').text(), /CRC failed/);
assert.equal($('#unzipStatusIndicator img').length, 0, 'errors are also rendered as text');
assert.equal(fileListRequests, 3, 'terminal errors refresh files extracted before the failure');
display(report({state: 'error', error: 'Carol.xlz: CRC failed', archivesFailed: 1}));
assert.equal($('#unzipStatusTitle').text(), 'Extraction needs attention', 'mixed batches do not accuse the last successful archive');
assert.equal(fileListRequests, 3, 'persistent errors do not repeatedly fetch the file list');

// A single existing XJ poller keeps running after navigating away from Home.
$('#home').addClass('hidden');
$('#filemanagement').removeClass('hidden');
display(report());
window.RequestStatusUpdate();
window.RequestStatusUpdate();
assert.equal(requests.length, 1, 'only one request can be queued while its predecessor is pending');
assert.equal(requests[0].url, 'XJ');
requests[0].error('timeout');
assert.equal($('#unzipStatusConnection').hasClass('hidden'), false);
assert.match($('#unzipStatusConnection').text(), /Reconnecting/);
assert.equal($('#unzipStatusIndicator').hasClass('hidden'), false, 'a failed poll preserves the last extraction state');
requests.shift().complete();
window.RequestStatusUpdate();
assert.equal(requests.length, 1, 'polling resumes after a failed request');
requests[0].success({status: {system: {unzip: report({state: 'complete', filesExtracted: 2})}}});
assert.equal($('#unzipStatusConnection').hasClass('hidden'), true, 'a fresh status clears stale connection feedback');
assert.equal($('#unzipStatusDetail').text(), '2 files ready.');
requests.shift().complete();
Object.defineProperty(window.document, 'hidden', {value: true, configurable: true});
window.RequestStatusUpdate();
assert.equal(requests.length, 0, 'hidden browser documents do not accumulate background requests');

dom.window.close();
console.log('Extraction GUI regressions passed: waiting, progress, completion, errors, safe filenames, file refresh, cross-tab polling, and reconnects.');
