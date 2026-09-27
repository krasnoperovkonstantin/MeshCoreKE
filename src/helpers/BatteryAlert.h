#pragma once

#include "CommonCLI.h"
#include "TransportKeyStore.h"
#include "TxtDataHelpers.h"
#include <Utils.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Shared low-battery group alert logic for repeater and room server.
class BatteryAlert {
public:
  BatteryAlert(NodePrefs& prefs, mesh::MainBoard& board, mesh::Mesh& mesh,
               mesh::RTCClock& clock, mesh::PacketManager& manager,
               CommonCLICallbacks& callbacks, const TransportKey& scope)
      : _prefs(prefs), _board(board), _mesh(mesh), _clock(clock), _manager(manager),
        _callbacks(callbacks), _scope(scope) {}

  bool handleCommand(const char* command, char* reply) {
    if (strcmp(command, "get alert") == 0) {
      snprintf(reply, 160, "> %s, threshold %u mV, group %s, key %s",
               _prefs.battery_alert_enabled ? "on" : "off",
               (unsigned)_prefs.battery_alert_threshold_mv,
               _prefs.battery_alert_channel_name[0] ? _prefs.battery_alert_channel_name : "none",
               hasKey(_prefs.battery_alert_channel_key) ? "set" : "not set");
    } else if (strcmp(command, "test alert") == 0) {
      if (!isConfigured()) {
        strcpy(reply, "ERR - set alert.group and alert.groupkey first");
      } else if (send(_board.getBattMilliVolts())) {
        strcpy(reply, "OK - test alert queued (delivery not confirmed)");
      } else {
        strcpy(reply, "ERR - unable to queue test alert");
      }
    } else if (strncmp(command, "set alert ", 10) == 0) {
      const char* value = command + 10;
      if (strcmp(value, "off") == 0) {
        _prefs.battery_alert_enabled = 0;
        _callbacks.savePrefs();
        strcpy(reply, "OK - battery alert off");
      } else if (strcmp(value, "on") == 0) {
        if (!isConfigured()) {
          strcpy(reply, "ERR - set alert.group and alert.groupkey first");
        } else {
          _prefs.battery_alert_enabled = 1;
          _callbacks.savePrefs();
          strcpy(reply, "OK - battery alert on");
        }
      } else {
        strcpy(reply, "ERR - use on or off");
      }
    } else if (strncmp(command, "set alert.threshold ", 20) == 0) {
      char* end;
      const unsigned long threshold = strtoul(command + 20, &end, 10);
      if (*end != 0 || threshold == 0 || threshold > 65535UL) {
        strcpy(reply, "ERR - threshold must be 1..65535 mV");
      } else {
        _prefs.battery_alert_threshold_mv = threshold;
        _callbacks.savePrefs();
        snprintf(reply, 160, "OK - alert threshold %lu mV", threshold);
      }
    } else if (strncmp(command, "set alert.group ", 16) == 0) {
      const char* name = command + 16;
      const size_t len = strlen(name);
      if (len == 0 || len >= sizeof(_prefs.battery_alert_channel_name)) {
        strcpy(reply, "ERR - group name must be 1..31 bytes");
      } else {
        memcpy(_prefs.battery_alert_channel_name, name, len + 1);
        _callbacks.savePrefs();
        strcpy(reply, "OK - alert group name saved");
      }
    } else if (strncmp(command, "set alert.groupkey ", 19) == 0) {
      uint8_t key[KEY_SIZE];
      if (!parseKey(key, command + 19)) {
        strcpy(reply, "ERR - use 32 hex digits or 24-character Base64 channel key");
      } else {
        memcpy(_prefs.battery_alert_channel_key, key, sizeof(key));
        _callbacks.savePrefs();
        strcpy(reply, "OK - alert group key saved");
      }
    } else {
      return false;
    }
    return true;
  }

  void afterAdvert() {
    if (!_prefs.battery_alert_enabled) return;
    const uint16_t battery_mv = _board.getBattMilliVolts();
    if (battery_mv > 0 && battery_mv < _prefs.battery_alert_threshold_mv) send(battery_mv);
  }

private:
  static constexpr size_t KEY_SIZE = 16;

  NodePrefs& _prefs;
  mesh::MainBoard& _board;
  mesh::Mesh& _mesh;
  mesh::RTCClock& _clock;
  mesh::PacketManager& _manager;
  CommonCLICallbacks& _callbacks;
  const TransportKey& _scope;

  static bool hasKey(const uint8_t key[KEY_SIZE]) {
    for (size_t i = 0; i < KEY_SIZE; ++i) {
      if (key[i]) return true;
    }
    return false;
  }

  bool isConfigured() const {
    return _prefs.battery_alert_channel_name[0] != 0 && hasKey(_prefs.battery_alert_channel_key);
  }

  static int base64Digit(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    return -1;
  }

  static bool parseKey(uint8_t key[KEY_SIZE], const char* value) {
    const size_t len = strlen(value);
    if (len == KEY_SIZE * 2) {
      for (size_t i = 0; i < len; ++i) {
        if (!mesh::Utils::isHexChar(value[i])) return false;
      }
      return mesh::Utils::fromHex(key, KEY_SIZE, value) && hasKey(key);
    }
    if (len != 24 || value[22] != '=' || value[23] != '=') return false;

    uint16_t bits_value = 0;
    uint8_t bits_count = 0;
    size_t out = 0;
    for (size_t i = 0; i < 22; ++i) {
      const int digit = base64Digit(value[i]);
      if (digit < 0) return false;
      bits_value = (bits_value << 6) | digit;
      bits_count += 6;
      if (bits_count >= 8) {
        bits_count -= 8;
        key[out++] = (bits_value >> bits_count) & 0xff;
        bits_value &= (1U << bits_count) - 1;
      }
    }
    return out == KEY_SIZE && bits_value == 0 && hasKey(key);
  }

  bool send(uint16_t battery_mv) {
    if (!isConfigured()) return false;

    uint8_t data[5 + 112];
    const uint32_t timestamp = _clock.getCurrentTimeUnique();
    memcpy(data, &timestamp, 4);
    data[4] = (TXT_TYPE_PLAIN << 2);
    const size_t text_len = snprintf((char*)&data[5], sizeof(data) - 5,
                                     "%s: Battery low: %u mV",
                                     _prefs.node_name, (unsigned)battery_mv,
                                     (unsigned)_prefs.battery_alert_threshold_mv);

    mesh::GroupChannel channel;
    memset(channel.secret, 0, sizeof(channel.secret));
    memcpy(channel.secret, _prefs.battery_alert_channel_key, KEY_SIZE);
    mesh::Utils::sha256(channel.hash, sizeof(channel.hash), channel.secret, KEY_SIZE);
    mesh::Packet* packet = _mesh.createGroupDatagram(PAYLOAD_TYPE_GRP_TXT, channel, data, 5 + text_len);
    if (!packet) return false;

    const int queued = _manager.getOutboundTotal();
    if (_scope.isNull()) {
      const uint32_t delay_millis = 0;
      _mesh.sendFlood(packet, delay_millis, _prefs.path_hash_mode + 1);
    } else {
      uint16_t codes[2] = {_scope.calcTransportCode(packet), 0};
      _mesh.sendFlood(packet, codes, 0, _prefs.path_hash_mode + 1);
    }
    return _manager.getOutboundTotal() > queued;
  }
};
