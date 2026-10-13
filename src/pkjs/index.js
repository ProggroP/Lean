// PebbleKit JS: reicht ausschliesslich die Clay-Einstellungen an die Uhr
// weiter. Die Zeit liest die Uhr selbst -- im laufenden Betrieb gibt es
// deshalb keinerlei Datenverkehr zum Telefon.

var Clay = require('@rebble/clay');
var clayConfig = require('./config');

var clay = new Clay(clayConfig);
