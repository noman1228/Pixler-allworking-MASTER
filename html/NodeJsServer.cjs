const express = require("express");
const http = require("http");
const { URL } = require('url');
const app = express();
const espsIp = "http://192.168.5.100/";

app.listen(9000, () => {
    console.log("Application started and Listening on port 9000");
});

app.use(express.static(__dirname));

app.get("/", (req, res) => {
    res.sendFile(__dirname + "/index.html");
});

// Forward /file/* requests to the ESP device preserving method and body
function proxyToEsps(req, res) {
    try {
        const target = new URL(espsIp);
        const options = {
            hostname: target.hostname,
            port: target.port || 80,
            path: req.originalUrl,
            method: req.method,
            headers: req.headers
        };

        const proxyReq = http.request(options, function (proxyRes) {
            res.writeHead(proxyRes.statusCode, proxyRes.headers);
            proxyRes.pipe(res, { end: true });
        });

        proxyReq.on('error', function (err) {
            console.error('Proxy request error:', err);
            res.sendStatus(502);
        });

        req.pipe(proxyReq, { end: true });
    }
    catch (err) {
        console.error('Proxy error:', err);
        res.sendStatus(500);
    }
}

// Use a named wildcard parameter to avoid path-to-regexp errors
// Mount middleware at /file to forward any nested paths without using path-to-regexp named wildcards
app.use('/file', (req, res) => { proxyToEsps(req, res); });
app.use('/api/testmode', (req, res) => { proxyToEsps(req, res); });

app.get("/V1", (req, res) => { res.redirect(espsIp + "V1"); });
app.get("/XP", (req, res) => { res.redirect(espsIp + "XP"); });
app.post("/X6", (req, res) => { res.redirect(espsIp + "X6"); });
app.post("/X7", (req, res) => { res.redirect(espsIp + "X7"); });
app.post("/XJ", (req, res) => { res.redirect(espsIp + "XJ"); });
app.post("/XP",          (req, res) => {res.redirect(espsIp + "XP")});
app.get("/XJ",           (req, res) => {res.redirect(espsIp + "XJ")});
app.put("/conf/:file",   (req, res) => {res.redirect(espsIp + "conf/" + req.params.file);});
app.get("/conf/:file",   (req, res) => {res.redirect(espsIp + "conf/" + req.params.file);});
app.post("/settime",     (req, res) => {res.redirect(espsIp + "settime?newtime=" + req.query.newtime);});
app.get("/fseqfilelist", (req, res) => {res.redirect(espsIp + "fseqfilelist");});
