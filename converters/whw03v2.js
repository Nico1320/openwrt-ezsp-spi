// External converter: Linksys WHW03 V2 (Velop) running OpenWrt as a Zigbee
// router, driven by ezsp-spi.
//
// Place at: <config>/external_converters/whw03v2.js and restart. Every .js in
// that folder is auto-loaded; no configuration.yaml entry is needed.
//
// The node exposes one Home Automation endpoint with Basic and Identify.
// Identify blinks the LEDs named in the router's identify_led setting, which
// is how you find which physical unit a device entry belongs to.

const m = require('zigbee-herdsman-converters/lib/modernExtend');
const exposes = require('zigbee-herdsman-converters/lib/exposes');
const e = exposes.presets;
const ea = exposes.access;

// Basic 0x0010 LocationDescription carries the router's hostname. Every node
// reports the same model, so this is what tells them apart.
const fzLocation = {
    cluster: 'genBasic',
    type: ['attributeReport', 'readResponse'],
    convert: (model, msg) => {
        if (msg.data.locationDesc !== undefined) {
            return {location: msg.data.locationDesc};
        }
    },
};

const tzLocation = {
    key: ['location'],
    convertGet: async (entity) => {
        await entity.read('genBasic', ['locationDesc']);
    },
};

module.exports = [
    {
        zigbeeModel: ['WHW03V2'],
        model: 'WHW03V2',
        vendor: 'Linksys',
        description: 'Velop node running OpenWrt as a Zigbee router (ezsp-spi)',

        extend: [m.identify()],

        fromZigbee: [fzLocation],
        toZigbee: [tzLocation],

        exposes: [
            e.text('location', ea.STATE_GET).withDescription(
                'Hostname of the router, read from the Basic cluster'),
        ],

        configure: async (device, coordinatorEndpoint) => {
            const ep = device.getEndpoint(1);
            await ep.read('genBasic', ['locationDesc']).catch(() => {});
        },
    },
];

// Note: do not add e.linkquality() here. It is appended to every device
// automatically, and the preset no longer exists in current releases -- calling
// it throws and invalidates the whole definition. A blank reading just means no
// message has arrived since the last restart; it fills in on the next report.
