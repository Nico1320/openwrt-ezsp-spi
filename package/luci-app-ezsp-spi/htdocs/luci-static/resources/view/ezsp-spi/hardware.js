'use strict';
'require view';
'require form';

return view.extend({
	render: function() {
		var m, s, o;

		m = new form.Map('ezsp-spi', _('Zigbee Hardware'),
			_('Autodetected. Leave empty unless your board is unknown.'));

		s = m.section(form.NamedSection, 'radio', 'radio', _('Mode'));

		o = s.option(form.Flag, 'enabled', _('Enabled'));
		o.default = '1';
		o.rmempty = false;

		o = s.option(form.ListValue, 'mode', _('Mode'));
		o.value('router', _('Router'));
		o.value('bridge', _('Bridge'));
		o.default = 'router';

		s = m.section(form.NamedSection, 'bridge', 'bridge', _('Bridge'),
			_('No authentication.'));

		o = s.option(form.Value, 'port', _('Port'));
		o.datatype = 'port';
		o.placeholder = '8888';
		o.optional = true;

		o = s.option(form.Value, 'bind', _('Listen address'));
		o.datatype = 'ipaddr';
		o.placeholder = '0.0.0.0';
		o.optional = true;

		s = m.section(form.NamedSection, 'network', 'network', _('Network'),
			_('The network to join. Take both from your coordinator.'));

		o = s.option(form.Value, 'channel', _('Channel'));
		o.datatype = 'range(11,26)';
		o.placeholder = '25';

		o = s.option(form.Value, 'pan_id', _('PAN ID'),
			_('Decimal. 0x1338 is 4920.'));
		o.datatype = 'range(0,65535)';
		o.placeholder = '4920';

		s = m.section(form.NamedSection, 'radio', 'radio', _('Interface'));

		o = s.option(form.Value, 'spidev', _('SPI device'));
		o.placeholder = '/dev/spidev1.0';

		o = s.option(form.Value, 'gpiochip', _('GPIO chip'));
		o.placeholder = '/dev/gpiochip0';

		o = s.option(form.Value, 'speed', _('SPI clock (Hz)'),
			_('Maximum 5 MHz.'));
		o.datatype = 'uinteger';
		o.placeholder = '1000000';
		o.optional = true;

		s = m.section(form.NamedSection, 'radio', 'radio', _('Pins'),
			_('GPIO chip offsets. Overrides detection.'));

		o = s.option(form.Value, 'reset', _('nRESET'));
		o.datatype = 'uinteger';
		o.placeholder = '49';
		o.optional = true;

		o = s.option(form.Value, 'wake', _('nWAKE'));
		o.datatype = 'uinteger';
		o.placeholder = '31';
		o.optional = true;

		o = s.option(form.Value, 'hostint', _('nHOST_INT'));
		o.datatype = 'uinteger';
		o.placeholder = '50';
		o.optional = true;

		o = s.option(form.Value, 'nssel_int', _('nSSEL_INT'));
		o.datatype = 'uinteger';
		o.placeholder = '29';
		o.optional = true;

		o = s.option(form.ListValue, 'nssel_int_mode', _('nSSEL_INT mode'));
		o.value('follow', _('Follow nSSEL (default)'));
		o.value('input', _('Leave alone'));
		o.value('low', _('Pin low'));
		o.value('high', _('Pin high'));

		s = m.section(form.NamedSection, 'radio', 'radio', _('Reporting'));

		o = s.option(form.Value, 'report_secs', _('Report interval (s)'),
			_('0 disables. Jittered ±20%.'));
		o.datatype = 'uinteger';
		o.placeholder = '300';
		o.optional = true;

		s = m.section(form.NamedSection, 'radio', 'radio', _('Protocol'));

		o = s.option(form.ListValue, 'ezsp_version', _('EZSP version'),
			_('Match your NCP to avoid an extra reset.'));
		o.value('4', _('4 (EmberZNet 5.8.x)'));
		o.value('5', _('5 (EmberZNet 5.10.x)'));
		o.value('8', _('8 (EmberZNet 6.7.x)'));

		return m.render();
	}
});
