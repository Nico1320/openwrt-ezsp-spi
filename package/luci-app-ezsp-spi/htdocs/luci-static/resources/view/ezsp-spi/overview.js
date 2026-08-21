'use strict';
'require view';
'require rpc';
'require poll';
'require ui';

var callStatus = rpc.declare({ object: 'ezsp', method: 'status', expect: { } });
var callJoin   = rpc.declare({ object: 'ezsp', method: 'join',   expect: { },
                               params: [ 'channel', 'pan_id' ] });
var callLeave  = rpc.declare({ object: 'ezsp', method: 'leave',  expect: { } });
var callScan   = rpc.declare({ object: 'ezsp', method: 'scan',   expect: { } });

var history = [];
var lastSent = null, lastFailed = null;
var HISTORY_MAX = 60;

var CSS = '\
.ez-wrap{--ez-ok:#1f9d55;--ez-warn:#d97706;--ez-bad:#dc2626;--ez-dim:#6b7280;\
 --ez-line:rgba(128,128,128,.22);--ez-card:rgba(128,128,128,.06)}\
.ez-hero{display:flex;align-items:center;gap:1rem;flex-wrap:wrap;\
 padding:1.1rem 1.25rem;border:1px solid var(--ez-line);border-radius:12px;\
 background:var(--ez-card);margin-bottom:1rem}\
.ez-dot{width:14px;height:14px;border-radius:50%;flex:0 0 auto;\
 box-shadow:0 0 0 4px rgba(31,157,85,.15)}\
.ez-dot.live{background:var(--ez-ok);animation:ezpulse 2s ease-in-out infinite}\
.ez-dot.bad{background:var(--ez-bad);box-shadow:0 0 0 4px rgba(220,38,38,.15)}\
.ez-dot.idle{background:var(--ez-dim);box-shadow:0 0 0 4px rgba(107,114,128,.15)}\
@keyframes ezpulse{0%,100%{opacity:1}50%{opacity:.45}}\
.ez-hero-main{flex:1 1 16rem;min-width:0}\
.ez-hero-title{font-size:1.35rem;font-weight:650;line-height:1.2}\
.ez-hero-sub{color:var(--ez-dim);font-size:.9rem;margin-top:.15rem}\
.ez-chip{display:inline-block;padding:.15rem .6rem;border-radius:999px;\
 font-size:.78rem;font-weight:600;letter-spacing:.02em;\
 border:1px solid var(--ez-line)}\
.ez-chip.router{background:rgba(31,157,85,.14);color:var(--ez-ok)}\
.ez-chip.bridge{background:rgba(37,99,235,.14);color:#2563eb}\
.ez-grid{display:grid;gap:.75rem;\
 grid-template-columns:repeat(auto-fit,minmax(9.5rem,1fr));margin-bottom:1rem}\
.ez-tile{padding:.8rem .9rem;border:1px solid var(--ez-line);border-radius:10px;\
 background:var(--ez-card)}\
.ez-tile-k{font-size:.72rem;text-transform:uppercase;letter-spacing:.06em;\
 color:var(--ez-dim);margin-bottom:.3rem}\
.ez-tile-v{font-size:1.15rem;font-weight:600;font-variant-numeric:tabular-nums;\
 word-break:break-all}\
.ez-tile-v small{font-size:.8rem;font-weight:400;color:var(--ez-dim)}\
.ez-health{display:flex;align-items:center;gap:1rem;flex-wrap:wrap;\
 padding:.9rem 1rem;border:1px solid var(--ez-line);border-radius:10px;\
 background:var(--ez-card);margin-bottom:1rem}\
.ez-spark{flex:1 1 12rem;min-width:10rem;height:44px}\
.ez-rate{font-size:1.6rem;font-weight:650;font-variant-numeric:tabular-nums}\
.ez-muted{color:var(--ez-dim);font-size:.85rem}\
.ez-actions{display:flex;gap:.5rem;flex-wrap:wrap;align-items:center}\
.ez-note{border-left:3px solid var(--ez-warn);padding:.5rem .75rem;\
 background:rgba(217,119,6,.08);border-radius:0 6px 6px 0;font-size:.88rem;\
 margin-bottom:.75rem}\
';

function tile(key, label, value, sub) {
	return E('div', { 'class': 'ez-tile' }, [
		E('div', { 'class': 'ez-tile-k' }, [ label ]),
		E('div', { 'class': 'ez-tile-v', 'data-ez': key }, [
			value != null && value !== '' ? String(value) : '—',
			sub ? E('small', {}, [ ' ' + sub ]) : ''
		])
	]);
}

function setTile(root, key, value, sub) {
	var el = root.querySelector('[data-ez="' + key + '"]');
	if (!el) return;
	var txt = (value != null && value !== '') ? String(value) : '—';
	if (el.getAttribute('data-v') === txt + '|' + (sub || '')) return;
	el.setAttribute('data-v', txt + '|' + (sub || ''));
	el.innerHTML = '';
	el.appendChild(document.createTextNode(txt));
	if (sub) el.appendChild(E('small', {}, [ ' ' + sub ]));
}

function sparkline(data) {
	var w = 240, h = 44, n = Math.max(data.length, 1);
	var bw = Math.max(1.5, (w / Math.max(n, 24)) - 1.5);
	var bars = data.map(function(d, i) {
		var x = i * (bw + 1.5);
		var mag = Math.min(d.sent, 6);
		var bh = d.sent ? Math.max(4, (mag / 6) * (h - 6)) : 2;
		return E('rect', {
			'x': x.toFixed(1), 'y': (h - bh).toFixed(1),
			'width': bw.toFixed(1), 'height': bh.toFixed(1), 'rx': '1',
			'fill': d.failed ? 'var(--ez-bad)' : 'var(--ez-ok)',
			'opacity': d.sent ? '0.9' : '0.25'
		});
	});
	return E('svg', {
		'class': 'ez-spark', 'viewBox': '0 0 ' + w + ' ' + h,
		'preserveAspectRatio': 'none'
	}, bars.length ? bars : [
		E('rect', { 'x': 0, 'y': h - 2, 'width': w, 'height': 2,
			    'fill': 'var(--ez-dim)', 'opacity': '.3' })
	]);
}

function pushHistory(st) {
	var s = parseInt(st.reports_sent, 10);
	var f = parseInt(st.reports_failed, 10);
	if (isNaN(s) || isNaN(f)) return;

	if (lastSent !== null) {
		var ds = s - lastSent, df = f - lastFailed;
		if (ds < 0 || df < 0) { history = []; }
		else if (ds || df) history.push({ sent: ds, failed: df });
		if (history.length > HISTORY_MAX) history.shift();
	}
	lastSent = s; lastFailed = f;
}

function heroFor(st) {
	var bridge = (st.mode === 'bridge');
	var joined = (st.state === 'joined');
	var cls, title, sub;

	if (!st.running) {
		cls = 'idle';
		title = _('Service stopped');
		sub = st.enabled ? _('The daemon is not running.')
				 : _('Disabled in configuration.');
	} else if (bridge) {
		cls = 'live';
		title = _('Bridge active');
		sub = _('Serving on %s:%s')
			.format(st.bridge_bind || '0.0.0.0', st.bridge_port || 8888);
	} else if (joined) {
		cls = 'live';
		title = _('Routing');
		sub = _('Joined and repeating for the network.');
	} else {
		cls = 'bad';
		title = _('Not joined');
		sub = _('Not a member of any network.');
	}

	return E('div', { 'class': 'ez-hero' }, [
		E('div', { 'class': 'ez-dot ' + cls, 'data-ez': 'dot' }),
		E('div', { 'class': 'ez-hero-main' }, [
			E('div', { 'class': 'ez-hero-title', 'data-ez': 'title' }, [ title ]),
			E('div', { 'class': 'ez-hero-sub', 'data-ez': 'sub' }, [ sub ])
		]),
		E('span', { 'class': 'ez-chip ' + (bridge ? 'bridge' : 'router'),
			    'data-ez': 'mode' }, [ bridge ? _('BRIDGE') : _('ROUTER') ])
	]);
}

function updateHero(root, st) {
	var bridge = (st.mode === 'bridge');
	var joined = (st.state === 'joined');
	var dot = root.querySelector('[data-ez="dot"]');
	var cls = !st.running ? 'idle' : (bridge || joined) ? 'live' : 'bad';
	if (dot) dot.className = 'ez-dot ' + cls;

	var chip = root.querySelector('[data-ez="mode"]');
	if (chip) {
		chip.className = 'ez-chip ' + (bridge ? 'bridge' : 'router');
		chip.textContent = bridge ? _('BRIDGE') : _('ROUTER');
	}
}

function healthPanel(st) {
	return E('div', { 'class': 'ez-health' }, [
		E('div', {}, [
			E('div', { 'class': 'ez-rate', 'data-ez': 'rate' }, [ '—' ]),
			E('div', { 'class': 'ez-muted' }, [ _('report success') ])
		]),
		E('div', { 'id': 'ez-spark-host', 'style': 'flex:1 1 12rem' }, [
			sparkline(history)
		]),
		E('div', {}, [
			E('div', { 'class': 'ez-tile-k' }, [ _('Last report') ]),
			E('div', { 'style': 'font-weight:600', 'data-ez': 'last' }, [ '—' ])
		])
	]);
}

function updateHealth(root, st) {
	// Counters are meaningless once the node is off the network; leaving them
	// on screen reads as if it were still reporting.
	if (st.state !== 'joined') {
		history = [];
		lastSent = null;
		lastFailed = null;
	}

	var joined = (st.state === 'joined');
	var s = parseInt(st.reports_sent, 10) || 0;
	var f = parseInt(st.reports_failed, 10) || 0;
	var rate = (joined && s) ? Math.round(((s - f) / s) * 100) : null;

	var el = root.querySelector('[data-ez="rate"]');
	if (el) {
		el.textContent = (rate === null) ? '—' : rate + '%';
		el.style.color = (rate === null) ? '' :
			(rate >= 99 ? 'var(--ez-ok)' :
			 rate >= 90 ? 'var(--ez-warn)' : 'var(--ez-bad)');
	}

	el = root.querySelector('[data-ez="last"]');
	if (el) el.textContent = joined ? (st.last_report || '—') : '—';

	var host = root.querySelector('#ez-spark-host');
	if (!host) return;
	if (host) { host.innerHTML = ''; host.appendChild(sparkline(history)); }
}

function joinNetwork(ch, pan) {
	ui.showModal(_('Please wait'), [
		E('p', { 'class': 'spinning' },
			[ _('Joining channel %s, PAN %s…').format(ch, pan) ])
	]);

	return callJoin(String(ch), String(parseInt(pan, 16))).then(function(res) {
		ui.hideModal();
		ui.showModal(_('Result'), [
			E('pre', { 'style': 'max-height:24em;overflow:auto' },
				[ (res && res.output) || _('No output') ]),
			E('div', { 'class': 'right' }, [
				E('button', { 'class': 'btn cbi-button',
					      'click': ui.hideModal }, [ _('Close') ])
			])
		]);
	}).catch(function(err) {
		ui.hideModal();
		ui.addNotification(null, E('p', {}, [ '' + err ]), 'error');
	});
}

// Every router and coordinator on a network answers a beacon request, so a scan
// returns one entry per device, not per network. Collapse them: the beacon
// carries no sender id, so devices are only distinguishable by signal.
function dedupe(list) {
	var by = {};

	(list || []).forEach(function(n) {
		var k = n.channel + '/' + n.pan_id;
		if (!by[k]) {
			by[k] = { channel: n.channel, pan_id: n.pan_id,
				  ext_pan_id: n.ext_pan_id, joinable: n.joinable,
				  lqi: n.lqi, rssi: n.rssi, seen: 1 };
			return;
		}
		by[k].seen++;
		by[k].joinable = by[k].joinable || n.joinable;
		if (n.lqi > by[k].lqi) { by[k].lqi = n.lqi; by[k].rssi = n.rssi; }
	});

	return Object.keys(by).map(function(k) { return by[k]; });
}

function scanRows(list) {
	var nets = dedupe(list);

	if (!nets.length)
		return [ E('tr', {}, [ E('td', { 'colspan': '7',
			'style': 'padding:1rem;opacity:.7' },
			[ _('No networks heard.') ]) ]) ];

	nets.sort(function(a, b) { return b.lqi - a.lqi; });

	return nets.map(function(n) {
		return E('tr', { 'class': 'tr' }, [
			E('td', { 'class': 'td' }, [ String(n.channel) ]),
			E('td', { 'class': 'td' }, [ n.pan_id ]),
			E('td', { 'class': 'td',
				  'style': 'font-family:ui-monospace,monospace;font-size:.85em' },
				[ n.ext_pan_id || '—' ]),
			E('td', { 'class': 'td' }, [ n.lqi + ' / ' + n.rssi + ' dBm' ]),
			E('td', { 'class': 'td' }, [ String(n.seen) ]),
			E('td', { 'class': 'td' }, [ n.joinable
				? E('span', { 'style': 'color:var(--ez-ok);font-weight:600' },
					[ _('open') ])
				: E('span', { 'style': 'opacity:.6' }, [ _('closed') ]) ]),
			E('td', { 'class': 'td' }, [
				E('button', {
					'class': 'btn cbi-button cbi-button-apply',
					'disabled': n.joinable ? null : '',
					'click': ui.createHandlerFn(this, function() {
						return joinNetwork(n.channel, n.pan_id);
					})
				}, [ _('Join') ])
			])
		]);
	});
}

function doScan() {
	ui.showModal(_('Scanning'), [
		E('p', { 'class': 'spinning' },
			[ _('Takes the radio off the network for a few seconds…') ])
	]);

	var nets = [];

	// The scan stops the daemon, so status is only readable once it is back.
	return callScan().then(function(res) {
		nets = (res && res.networks) || [];
		return callStatus();
	}).then(function(st) {
		st = st || {};
		ui.hideModal();
		ui.showModal(_('Networks heard'), [
			E('p', { 'style': 'opacity:.75;font-size:.9em' }, [
				_('This node: %s  %s').format(
					st.node_id || _('not joined'), st.eui64 || '')
			]),
			E('table', { 'class': 'table' }, [
				E('tr', { 'class': 'tr table-titles' }, [
					E('th', { 'class': 'th' }, [ _('Channel') ]),
					E('th', { 'class': 'th' }, [ _('PAN ID') ]),
					E('th', { 'class': 'th' }, [ _('Extended PAN') ]),
					E('th', { 'class': 'th' }, [ _('LQI / RSSI') ]),
					E('th', { 'class': 'th' }, [ _('Devices') ]),
					E('th', { 'class': 'th' }, [ _('Join') ]),
					E('th', { 'class': 'th' }, [ '' ])
				])
			].concat(scanRows(nets))),
			E('div', { 'class': 'right' }, [
				E('button', { 'class': 'btn cbi-button',
					      'click': ui.hideModal }, [ _('Close') ])
			])
		]);
	}).catch(function(err) {
		ui.hideModal();
		ui.addNotification(null, E('p', {}, [ '' + err ]), 'error');
	});
}

function action(fn, busyText, confirmText) {
	return function() {
		if (confirmText && !confirm(confirmText))
			return;

		ui.showModal(_('Please wait'), [
			E('p', { 'class': 'spinning' }, [ busyText ])
		]);

		return fn().then(function(res) {
			ui.hideModal();
			ui.showModal(_('Result'), [
				E('pre', { 'style': 'max-height:24em;overflow:auto' },
					[ (res && res.output) || _('No output') ]),
				E('div', { 'class': 'right' }, [
					E('button', { 'class': 'btn cbi-button',
						      'click': ui.hideModal }, [ _('Close') ])
				])
			]);
		}).catch(function(err) {
			ui.hideModal();
			ui.addNotification(null, E('p', {}, [ '' + err ]), 'error');
		});
	};
}

return view.extend({
	load: function() {
		return callStatus().catch(function() { return {}; });
	},

	render: function(st) {
		st = st || {};
		pushHistory(st);

		var bridge = (st.mode === 'bridge');

		var clients = st.clients || [];
		var tiles = bridge ? E('div', { 'class': 'ez-grid' }, [
			tile('listen',  _('Listening on'),
				(st.bridge_bind || '0.0.0.0') + ':' + (st.bridge_port || 8888)),
			tile('lstate',  _('Socket'),   st.listening ? _('open') : _('closed')),
			tile('nclient', _('Clients'),  clients.length),
			tile('client',  _('Connected'), clients.length ? clients.join(', ') : _('none'))
		]) : E('div', { 'class': 'ez-grid' }, [
			tile('node_id',  _('Node ID'),      st.node_id),
			tile('pan_id',   _('PAN ID'),       st.pan_id),
			tile('channel',  _('Channel'),      st.channel),
			tile('tx_power', _('TX power'),     st.tx_power, 'dBm'),
			tile('node_type',_('Node type'),    st.node_type),
			tile('eui64',    _('IEEE address'), st.eui64)
		]);

		var root = E('div', { 'class': 'ez-wrap' }, [
			E('style', {}, [ CSS ]),
			heroFor(st),
			tiles,
			bridge ? '' : E('div', { 'class': 'cbi-section' }, [
				E('h3', {}, [ _('Link health') ]),
				healthPanel(st)
			]),
			E('div', { 'class': 'cbi-section' }, [
				E('h3', {}, [ _('Actions') ]),
				bridge ? E('div', { 'class': 'ez-note' }, [
					_('Handled by the connected client.')
				]) : E('p', { 'class': 'ez-muted' }, [
					_('Open permit-join on the coordinator first.')
				]),
				E('div', { 'class': 'ez-actions' }, [
					E('button', {
						'class': 'btn cbi-button cbi-button-action',
						'disabled': bridge ? '' : null,
						'click': ui.createHandlerFn(this, doScan)
					}, [ _('Scan and join') ]),
					E('button', {
						'class': 'btn cbi-button cbi-button-apply',
						'disabled': bridge ? '' : null,
						'click': action(callJoin, _('Joining the network…'))
					}, [ _('Join configured') ]),
					E('button', {
						'class': 'btn cbi-button cbi-button-reset',
						'disabled': bridge ? '' : null,
						'click': action(callLeave, _('Leaving the network…'),
							_('Leave the network and clear stored state?'))
					}, [ _('Leave network') ])
				])
			])
		]);

		updateHealth(root, st);

		poll.add(function() {
			return callStatus().then(function(cur) {
				cur = cur || {};
				pushHistory(cur);
				updateHero(root, cur);
				if (cur.mode === 'bridge') {
					var cl = cur.clients || [];
					setTile(root, 'listen', (cur.bridge_bind || '0.0.0.0') + ':' + (cur.bridge_port || 8888));
					setTile(root, 'lstate', cur.listening ? _('open') : _('closed'));
					setTile(root, 'nclient', cl.length);
					setTile(root, 'client', cl.length ? cl.join(', ') : _('none'));
				} else {
					setTile(root, 'node_id',   cur.node_id);
					setTile(root, 'pan_id',    cur.pan_id);
					setTile(root, 'channel',   cur.channel);
					setTile(root, 'tx_power',  cur.tx_power, 'dBm');
					setTile(root, 'node_type', cur.node_type);
					setTile(root, 'eui64',     cur.eui64);
				}
				updateHealth(root, cur);

				var t = root.querySelector('[data-ez="title"]');
				var s = root.querySelector('[data-ez="sub"]');
				var fresh = heroFor(cur);
				if (t) t.textContent = fresh.querySelector('[data-ez="title"]').textContent;
				if (s) s.textContent = fresh.querySelector('[data-ez="sub"]').textContent;
			});
		}, 5);

		return Promise.resolve(root);
	},

	handleSave: null,
	handleSaveApply: null,
	handleReset: null
});
