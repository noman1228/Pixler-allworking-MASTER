// Run with node tests/testmode/count-ui.cjs. Uses the local jsdom dependency.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const {createRequire} = require('node:module');
const root = path.resolve(__dirname, '../..');
const localRequire = createRequire(path.join(root, '.pio/ui-verification/package.json'));
const {JSDOM} = localRequire('jsdom');
const html = fs.readFileSync(path.join(root, 'html/index.html'), 'utf8');
const source = fs.readFileSync(path.join(root, 'html/js/script.js'), 'utf8');

function harness(overrides = {}) {
    const dom = new JSDOM(html, {url: 'http://controller.local/', runScripts: 'outside-only', pretendToBeVisual: true});
    const {window} = dom;
    window.eval(fs.readFileSync(path.join(root, 'html/js/jquery-3.7.1.js'), 'utf8'));
    const $ = window.jQuery;
    const requests = [];
    const intervals = [];
    const timeouts = new Map();
    let nextTimer = 1;
    let time = 100000;
    window.Date.now = () => time;
    window.setInterval = callback => intervals.push(callback);
    window.setTimeout = callback => {
        const id = nextTimer++;
        timeouts.set(id, callback);
        return id;
    };
    window.clearTimeout = id => timeouts.delete(id);
    $.expr.pseudos.visible = element => !$(element).closest('.hidden').length;
    $.ajax = options => {
        const deferred = $.Deferred();
        requests.push({options, deferred});
        return deferred.promise();
    };
    const state = {
        active: false, pattern: 'red', output: -1, pixel: 1, brightness: 64, interval: 250, segment: 5,
        outputs: [{id: 0, name: 'WS2811', pixels: 3}, {id: 2, name: 'RGBW', pixels: 10}], totalPixels: 13,
        ...overrides
    };
    function reply(changes = {}) {
        const request = requests.shift();
        assert.ok(request, 'a request must be pending');
        Object.assign(state, changes);
        request.deferred.resolve(JSON.parse(JSON.stringify(state)));
        return request.options;
    }
    function input(id, value, event = 'input') {
        $('#' + id).val(value).trigger(event);
    }
    function settleTimers() {
        const pending = [...timeouts.values()];
        timeouts.clear();
        pending.forEach(callback => callback());
    }
    function poll() {
        time += 5000;
        intervals[0]();
    }
    function fail() {
        const request = requests.shift();
        assert.ok(request, 'a request must be pending');
        request.deferred.reject({status: 0}, 'timeout');
        return request.options;
    }
    window.eval(source.slice(source.indexOf('const TestModeUI = (function () {')) + '\nTestModeUI.initialize();');
    assert.equal(reply().type, 'GET');
    return {dom, $, requests, state, reply, input, settleTimers, poll, fail};
}

function countRange($, start, end) {
    const text = $('#testmode-count-range').text();
    assert.match(text, new RegExp('\\b' + start + '\\b'), 'range shows the first white LED');
    assert.match(text, new RegExp('\\b' + end + '\\b'), 'range shows the last white LED');
}

{
    const {dom, $, requests, reply, input, settleTimers, poll} = harness();
    assert.equal($('#testmode-pattern option[value="count"]').length, 1, 'LED counting is a separate test');
    input('testmode-pattern', 'count', 'change');
    assert.equal($('#testmode-output').val(), '0', 'counting selects a single configured output');
    assert.equal($('#testmode-count-position').attr('type'), 'range');
    assert.equal($('#testmode-count-size').attr('type'), 'range');
    assert.equal($('#testmode-count-position').attr('min'), '1');
    assert.equal($('#testmode-count-size').attr('min'), '1');
    assert.equal($('#testmode-count-size').attr('max'), '5');
    assert.equal($('#testmode-interval').prop('disabled'), true, 'manual counting has no animation speed');
    assert.equal($('#testmode-pixel').prop('disabled'), true, 'blinking LED control is separate');
    input('testmode-output', 2, 'change');
    input('testmode-count-size', 3);
    input('testmode-count-position', 1);
    countRange($, 1, 3);
    settleTimers();
    assert.equal(requests.length, 0, 'draft slider movement does not start an inactive test');
    $('#testmode-form').triggerHandler('submit');
    assert.equal(requests.length, 1);
    assert.equal(requests[0].options.data.pattern, 'count');
    assert.equal(requests[0].options.data.output, 2);
    assert.equal(requests[0].options.data.pixel, 1);
    assert.equal(requests[0].options.data.segment, 3);
    reply({active: true, pattern: 'count', output: 2, pixel: 1, segment: 3});
    assert.equal($('#testmode-banner').hasClass('hidden'), false);

    // An old in-flight status must not erase slider movement or consume the
    // command that should run as soon as the status request finishes.
    poll();
    assert.equal(requests.length, 1);
    assert.equal(requests[0].options.type, 'GET');
    input('testmode-count-position', 2);
    input('testmode-count-position', 4);
    input('testmode-count-position', 6);
    settleTimers();
    assert.equal(requests.length, 1, 'slider changes never overlap a request');
    reply();
    settleTimers();
    assert.equal($('#testmode-count-position').val(), '6', 'old status preserves the latest slider draft');
    assert.equal(requests.length, 1, 'coalesced slider update runs after the status reply');
    assert.equal(requests[0].options.type, 'POST');
    assert.equal(requests[0].options.data.pixel, 6, 'only the latest requested position is sent');
    assert.equal(requests[0].options.data.segment, 3);
    reply({pixel: 6});
    countRange($, 6, 8);

    // Changes made during a live update survive its response and are sent once.
    input('testmode-count-position', 3);
    settleTimers();
    assert.equal(requests.length, 1);
    assert.equal($('#testmode-count-position').prop('disabled'), false, 'dragging remains available during a live request');
    assert.equal($('#testmode-count-size').prop('disabled'), false, 'group size remains available during a live request');
    input('testmode-count-position', 4);
    input('testmode-count-size', 2);
    settleTimers();
    reply({pixel: 3});
    settleTimers();
    assert.equal($('#testmode-count-position').val(), '4');
    assert.equal($('#testmode-count-size').val(), '2');
    assert.equal(requests.length, 1);
    assert.equal(requests[0].options.data.pixel, 4);
    assert.equal(requests[0].options.data.segment, 2);
    reply({pixel: 4, segment: 2});

    // Stop must discard any queued movement, even if it was requested while
    // another live update was still in flight.
    input('testmode-count-position', 2);
    settleTimers();
    input('testmode-count-position', 5);
    settleTimers();
    $('.testmode-stop').first().trigger('click');
    reply({pixel: 2});
    settleTimers();
    assert.equal(requests.length, 1, 'Stop is the next command');
    assert.equal(requests[0].options.data.active, false);
    reply({active: false});
    settleTimers();
    assert.equal(requests.length, 0, 'queued slider movement cannot restart a stopped test');
    assert.equal($('#testmode-banner').hasClass('hidden'), true);
    input('testmode-count-position', 3);
    settleTimers();
    assert.equal(requests.length, 0, 'slider movement after Stop remains a draft');

    // The position is the first lit LED, including the physical last LED.
    // Group length shrinks at that edge rather than wrapping to the start.
    input('testmode-count-size', 5);
    input('testmode-count-position', 10);
    assert.equal($('#testmode-count-position').attr('max'), '10', 'position reaches the last configured LED');
    countRange($, 10, 10);
    $('#testmode-form').triggerHandler('submit');
    assert.equal(requests[0].options.data.pixel, 10);
    assert.equal(requests[0].options.data.segment, 5);
    reply({active: true, pattern: 'count', output: 2, pixel: 10, segment: 5});
    $('.testmode-stop').first().trigger('click');
    reply({active: false});
    input('testmode-output', 0, 'change');
    input('testmode-count-position', 1);
    assert.equal($('#testmode-count-position').attr('max'), '3', 'position bounds follow the selected output');
    assert.equal($('#testmode-count-size').attr('max'), '5', 'group-size slider retains the requested 1–5 range on short outputs');
    countRange($, 1, 3);
    input('testmode-count-position', 3);
    countRange($, 3, 3);
    $('#testmode-form').triggerHandler('submit');
    assert.equal(requests[0].options.data.output, 0);
    assert.equal(requests[0].options.data.pixel, 3);
    assert.equal(requests[0].options.data.segment, 5);
    reply({active: true, output: 0, pixel: 3, segment: 5});
    $('.testmode-stop').first().trigger('click');
    reply({active: false});
    input('testmode-pattern', 'red', 'change');
    assert.equal($('#testmode-count-position').prop('disabled'), true);
    assert.equal($('#testmode-count-size').prop('disabled'), true);
    dom.window.close();
}

{
    const {dom, $, requests, reply, input, settleTimers, poll} = harness({
        active: true, pattern: 'count', output: 2, pixel: 1, segment: 2
    });
    poll();
    input('testmode-count-position', 4);
    input('testmode-pattern', 'red', 'change');
    settleTimers();
    reply();
    settleTimers();
    assert.equal(requests.length, 0, 'leaving Count discards its queued slider movement without starting another pattern');
    assert.equal($('#testmode-pattern').val(), 'red', 'old status preserves the newly selected pattern');
    $('.testmode-stop').first().trigger('click');
    reply({active: false});
    dom.window.close();
}

{
    const {dom, $, requests, reply, input, settleTimers, fail} = harness({
        active: true, pattern: 'count', output: 2, pixel: 9, segment: 5
    });
    assert.equal($('#testmode-count-position').attr('max'), '10');
    assert.equal($('#testmode-count-position').val(), '9', 'opening a running count keeps its actual position near the end');
    assert.equal($('#testmode-count-size').val(), '5');
    countRange($, 9, 10);
    input('testmode-count-position', 8);
    assert.equal(requests.length, 1);
    input('testmode-count-position', 8, 'change');
    reply({pixel: 8});
    settleTimers();
    assert.equal(requests.length, 0, 'native input/change pair sends one command for the same position');
    input('testmode-count-position', 8, 'change');
    assert.equal(requests.length, 0, 'already confirmed position does not reapply the test');
    input('testmode-count-position', 7);
    input('testmode-count-position', 6);
    fail();
    settleTimers();
    assert.equal(requests.length, 0, 'a failed live request discards automatic queued movement');
    assert.equal($('#testmode-count-position').val(), '6', 'connection failure preserves the latest requested position');
    assert.equal($('#testmode-count-position').prop('disabled'), true, 'sliders wait for reconnection after an unknown result');
    assert.equal($('#testmode-banner').hasClass('hidden'), false, 'a lost response does not claim the running test stopped');
    $('.testmode-stop').first().trigger('click');
    assert.equal(requests[0].options.data.active, false, 'Stop is still usable after an uncertain live result');
    reply({active: false});
    dom.window.close();
}

{
    const {dom, $, requests, reply, input, settleTimers} = harness({
        active: true, pattern: 'count', output: 2, pixel: 1, segment: 2
    });
    for (const brightness of ['', 0, 256]) {
        input('testmode-brightness', brightness);
        assert.equal($('#testmode-form')[0].checkValidity(), false, 'brightness draft is invalid');
        input('testmode-count-position', 3);
        settleTimers();
        assert.equal(requests.length, 0, 'invalid brightness cannot be applied by moving a slider');
    }
    input('testmode-brightness', 64);
    assert.equal($('#testmode-form')[0].checkValidity(), true);
    input('testmode-count-position', 4);
    assert.equal(requests.length, 1, 'valid brightness permits live movement again');
    assert.equal(requests[0].options.data.pixel, 4);
    assert.equal(requests[0].options.data.brightness, 64);
    reply({pixel: 4, brightness: 64});
    $('.testmode-stop').first().trigger('click');
    reply({active: false});
    dom.window.close();
}

console.log('LED counting GUI regressions passed: sliders, range feedback, live movement, request coalescing, draft preservation, and Stop priority.');
