'use strict';
'require view';
'require rpc';
'require poll';

var callStatus    = rpc.declare({ object: 'ezsp', method: 'status',    expect: { } });
var callNeighbors = rpc.declare({ object: 'ezsp', method: 'neighbors', expect: { } });

var CSS = '\
.ezn{--ez-ok:#1f9d55;--ez-warn:#d97706;--ez-bad:#dc2626;--ez-dim:#6b7280;\
 --ez-line:rgba(128,128,128,.22);--ez-card:rgba(128,128,128,.06)}\
.ezn-card{border:1px solid var(--ez-line);border-radius:10px;\
 background:var(--ez-card);padding:1rem;margin-bottom:1rem}\
.ezn-tbl{width:100%;border-collapse:collapse;font-variant-numeric:tabular-nums}\
.ezn-tbl th{text-align:left;font-size:.72rem;text-transform:uppercase;\
 letter-spacing:.06em;color:var(--ez-dim);padding:.4rem .5rem;\
 border-bottom:1px solid var(--ez-line);font-weight:600}\
.ezn-tbl td{padding:.5rem;border-bottom:1px solid var(--ez-line)}\
.ezn-tbl tr:last-child td{border-bottom:0}\
.ezn-addr{font-family:ui-monospace,SFMono-Regular,Menlo,monospace;font-weight:600}\
.ezn-bar{position:relative;height:8px;border-radius:999px;\
 background:rgba(128,128,128,.18);overflow:hidden;min-width:5rem}\
.ezn-bar span{position:absolute;inset:0 auto 0 0;border-radius:999px;\
 transition:width .4s ease,background .4s ease}\
.ezn-cost{display:inline-block;min-width:1.6rem;text-align:center;\
 padding:.1rem .4rem;border-radius:5px;font-size:.8rem;font-weight:600}\
.ezn-empty{color:var(--ez-dim);font-style:italic;padding:1rem .5rem}\
.ezn-lead{color:var(--ez-dim);font-size:.88rem;margin:-.25rem 0 .85rem}\
.ezn-self{display:flex;gap:1.5rem;flex-wrap:wrap;margin-bottom:.85rem}\
.ezn-self div span{display:block;font-size:.72rem;text-transform:uppercase;\
 letter-spacing:.06em;color:var(--ez-dim)}\
.ezn-self div strong{font-size:1.05rem;font-variant-numeric:tabular-nums}\
';

function lqiColor(lqi) {
	if (lqi >= 200) return 'var(--ez-ok)';
	if (lqi >= 100) return 'var(--ez-warn)';
	return 'var(--ez-bad)';
}

function costColor(c) {
	if (!c) return 'rgba(128,128,128,.18)';
	if (c <= 1) return 'rgba(31,157,85,.18)';
	if (c <= 3) return 'rgba(217,119,6,.18)';
	return 'rgba(220,38,38,.18)';
}

function bar(lqi) {
	var pct = Math.max(0, Math.min(100, (lqi / 255) * 100));
	return E('div', { 'class': 'ezn-bar' }, [
		E('span', { 'style': 'width:' + pct.toFixed(0) + '%;background:' + lqiColor(lqi) })
	]);
}

function neighborRows(list) {
	if (!list || !list.length)
		return [ E('tr', {}, [ E('td', { 'colspan': '5', 'class': 'ezn-empty' },
			[ _('No neighbours.') ]) ]) ];

	return list.map(function(n) {
		return E('tr', {}, [
			E('td', { 'class': 'ezn-addr' }, [ n.addr ]),
			E('td', { 'style': 'width:40%' }, [ bar(n.lqi) ]),
			E('td', {}, [ String(n.lqi) ]),
			E('td', {}, [ E('span', {
				'class': 'ezn-cost',
				'style': 'background:' + costColor(n.in_cost)
			}, [ String(n.in_cost) ]) ]),
			E('td', {}, [ E('span', {
				'class': 'ezn-cost',
				'style': 'background:' + costColor(n.out_cost)
			}, [ String(n.out_cost) ]) ])
		]);
	});
}

function routeRows(list) {
	if (!list || !list.length)
		return [ E('tr', {}, [ E('td', { 'colspan': '3', 'class': 'ezn-empty' },
			[ _('No routes.') ]) ]) ];

	return list.map(function(r) {
		var direct = (r.dest === r.via);
		return E('tr', {}, [
			E('td', { 'class': 'ezn-addr' }, [ r.dest ]),
			E('td', { 'class': 'ezn-addr' }, [
				direct ? E('em', { 'style': 'color:var(--ez-dim);font-style:normal' },
					[ _('direct') ]) : r.via
			]),
			E('td', {}, [ r.state === 0 ? _('active') : String(r.state) ])
		]);
	});
}

function clientRows(list) {
	if (!list || !list.length)
		return [ E('tr', {}, [ E('td', { 'colspan': '2', 'class': 'ezn-empty' },
			[ _('No clients connected.') ]) ]) ];

	return list.map(function(c) {
		var i = c.lastIndexOf(':');
		return E('tr', {}, [
			E('td', { 'class': 'ezn-addr' }, [ i > 0 ? c.slice(0, i) : c ]),
			E('td', {}, [ i > 0 ? c.slice(i + 1) : '' ])
		]);
	});
}

function renderBridge(st) {
	var clients = st.clients || [];

	var root = E('div', { 'class': 'ezn' }, [
		E('style', {}, [ CSS ]),
		E('h2', {}, [ _('Zigbee network') ]),

		E('div', { 'class': 'ezn-card' }, [
			E('div', { 'class': 'ezn-self' }, [
				E('div', {}, [ E('span', {}, [ _('Mode') ]),
					E('strong', {}, [ _('Bridge') ]) ]),
				E('div', {}, [ E('span', {}, [ _('Listening') ]),
					E('strong', {}, [ (st.bridge_bind || '0.0.0.0') + ':' +
							  (st.bridge_port || 8888) ]) ]),
				E('div', {}, [ E('span', {}, [ _('Socket') ]),
					E('strong', { 'data-ezn': 'lstate' },
						[ st.listening ? _('open') : _('closed') ]) ]),
				E('div', {}, [ E('span', {}, [ _('Clients') ]),
					E('strong', { 'data-ezn': 'cnt' }, [ String(clients.length) ]) ])
			])
		]),

		E('div', { 'class': 'ezn-card' }, [
			E('h3', { 'style': 'margin-top:0' }, [ _('Connected clients') ]),
			E('p', { 'class': 'ezn-lead' }, [
				_('Managed by the client.')
			]),
			E('table', { 'class': 'ezn-tbl' }, [
				E('thead', {}, [ E('tr', {}, [
					E('th', {}, [ _('Address') ]),
					E('th', {}, [ _('Port') ])
				]) ]),
				E('tbody', { 'data-ezn': 'ctbody' }, clientRows(clients))
			])
		])
	]);

	poll.add(function() {
		return callStatus().then(function(cur) {
			cur = cur || {};
			var cl = cur.clients || [];
			var el = root.querySelector('[data-ezn="cnt"]');
			if (el) el.textContent = String(cl.length);
			el = root.querySelector('[data-ezn="lstate"]');
			if (el) el.textContent = cur.listening ? _('open') : _('closed');
			var tb = root.querySelector('[data-ezn="ctbody"]');
			if (tb) {
				tb.innerHTML = '';
				clientRows(cl).forEach(function(r) { tb.appendChild(r); });
			}
		});
	}, 5);

	return root;
}

return view.extend({
	load: function() {
		return Promise.all([
			callStatus().catch(function() { return {}; }),
			callNeighbors().catch(function() { return {}; })
		]);
	},

	render: function(data) {
		var st = data[0] || {}, nb = data[1] || {};

		if (st.mode === 'bridge')
			return Promise.resolve(renderBridge(st));

		var self = E('div', { 'class': 'ezn-self' }, [
			E('div', {}, [ E('span', {}, [ _('This node') ]),
				E('strong', { 'data-ezn': 'self' }, [ st.node_id || '—' ]) ]),
			E('div', {}, [ E('span', {}, [ _('PAN') ]),
				E('strong', { 'data-ezn': 'pan' }, [ st.pan_id || '—' ]) ]),
			E('div', {}, [ E('span', {}, [ _('Channel') ]),
				E('strong', { 'data-ezn': 'ch' }, [ st.channel || '—' ]) ]),
			E('div', {}, [ E('span', {}, [ _('Neighbours') ]),
				E('strong', { 'data-ezn': 'cnt' },
					[ String((nb.neighbors || []).length) ]) ])
		]);

		var ntbody = E('tbody', { 'data-ezn': 'ntbody' }, neighborRows(nb.neighbors));
		var rtbody = E('tbody', { 'data-ezn': 'rtbody' }, routeRows(nb.routes));

		var root = E('div', { 'class': 'ezn' }, [
			E('style', {}, [ CSS ]),
			E('h2', {}, [ _('Zigbee network') ]),

			E('div', { 'class': 'ezn-card' }, [ self ]),

			E('div', { 'class': 'ezn-card' }, [
				E('h3', { 'style': 'margin-top:0' }, [ _('Neighbours') ]),
				E('p', { 'class': 'ezn-lead' }, [
					_('Out cost: lower is better.')
				]),
				E('table', { 'class': 'ezn-tbl' }, [
					E('thead', {}, [ E('tr', {}, [
						E('th', {}, [ _('Address') ]),
						E('th', {}, [ _('LQI') ]),
						E('th', {}, [ '' ]),
						E('th', {}, [ _('In') ]),
						E('th', {}, [ _('Out') ])
					]) ]),
					ntbody
				])
			]),

			E('div', { 'class': 'ezn-card' }, [
				E('h3', { 'style': 'margin-top:0' }, [ _('Routes') ]),
				E('table', { 'class': 'ezn-tbl' }, [
					E('thead', {}, [ E('tr', {}, [
						E('th', {}, [ _('Destination') ]),
						E('th', {}, [ _('Via') ]),
						E('th', {}, [ _('State') ])
					]) ]),
					rtbody
				])
			])
		]);

		poll.add(function() {
			return Promise.all([ callStatus(), callNeighbors() ]).then(function(cur) {
				var s = cur[0] || {}, n = cur[1] || {};

				var set = function(k, v) {
					var el = root.querySelector('[data-ezn="' + k + '"]');
					if (el && el.textContent !== String(v || '—'))
						el.textContent = v || '—';
				};
				set('self', s.node_id);
				set('pan',  s.pan_id);
				set('ch',   s.channel);
				set('cnt',  String((n.neighbors || []).length));

				var nb2 = root.querySelector('[data-ezn="ntbody"]');
				if (nb2) {
					nb2.innerHTML = '';
					neighborRows(n.neighbors).forEach(function(r) { nb2.appendChild(r); });
				}
				var rb2 = root.querySelector('[data-ezn="rtbody"]');
				if (rb2) {
					rb2.innerHTML = '';
					routeRows(n.routes).forEach(function(r) { rb2.appendChild(r); });
				}
			});
		}, 10);

		return Promise.resolve(root);
	},

	handleSave: null,
	handleSaveApply: null,
	handleReset: null
});
