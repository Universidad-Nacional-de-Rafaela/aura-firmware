#ifndef ICIV_MESH_STORAGE_H
#define ICIV_MESH_STORAGE_H
#include <Preferences.h>
#include "mesh_core.h"

// Dos bancos por registro: escribir banco inactivo y despues confirmar indice.
// Un corte antes del indice no modifica la cola que sigue referenciada.
constexpr size_t RECORDS_PER_SENSOR = MESH_FIFO_CAP + 1;
constexpr size_t RECORD_COUNT = 2 * RECORDS_PER_SENSOR;
constexpr uint32_t INDEX_MAGIC = 0x48465231; // HFR1: formato local, sin migraciones.

#pragma pack(push, 1)
struct QueueIndex {
  uint32_t magic;
  uint32_t generation;
  MeshConfig config;
  uint8_t head[2];
  uint8_t count[2];
  uint8_t hasFirst[2];
  uint8_t firstAcked[2];
  uint32_t dropped[2];
  uint32_t missedCuts;
  uint32_t cutAt;
  uint8_t powerKnown;
  uint8_t onBattery;
  uint8_t capturePending;
  uint8_t sensorKnown[2];
  uint8_t sensorState[2];
  uint64_t banks;
  uint32_t recordCrc[RECORD_COUNT];
  uint32_t crc;
};

struct StoredRecord {
  Muestra sample;
  uint32_t crc;
};
#pragma pack(pop)

static_assert(sizeof(QueueIndex) < 360, "Indice menor que el journal");
static_assert(RECORD_COUNT <= 64, "Un bit de banco por registro");

class MeshStorage {
  Preferences prefs;
  QueueIndex active = {};
  QueueIndex candidate = {};
  Journal previous = {};
  uint8_t slot = 0;

  bool activeSlot(const Journal& j, int sensor, int position) {
    for (int i = 0; i < j.count[sensor]; ++i) {
      if ((j.head[sensor] + i) % MESH_FIFO_CAP == size_t(position)) {
        return true;
      }
    }
    return false;
  }

  void key(char out[12], int record, bool bank) {
    snprintf(out, 12, "r%02d%c", record, bank ? 'b' : 'a');
  }

  bool validIndex(const QueueIndex& index) {
    return index.magic == INDEX_MAGIC &&
      index.crc == crc32(reinterpret_cast<const uint8_t*>(&index), offsetof(QueueIndex, crc));
  }

  void restoreMetadata(const QueueIndex& index, Journal& j) {
    journalInit(j);
    j.generation = index.generation;
    j.config = index.config;
    memcpy(j.head, index.head, sizeof(j.head));
    memcpy(j.count, index.count, sizeof(j.count));
    memcpy(j.hasFirst, index.hasFirst, sizeof(j.hasFirst));
    memcpy(j.firstAcked, index.firstAcked, sizeof(j.firstAcked));
    memcpy(j.dropped, index.dropped, sizeof(j.dropped));
    j.missedCuts = index.missedCuts;
    j.cutAt = index.cutAt;
    j.powerKnown = index.powerKnown;
    j.onBattery = index.onBattery;
    j.capturePending = index.capturePending;
    memcpy(j.sensorKnown, index.sensorKnown, sizeof(j.sensorKnown));
    memcpy(j.sensorState, index.sensorState, sizeof(j.sensorState));
  }

  void storeMetadata(const Journal& j, QueueIndex& index) {
    index.magic = INDEX_MAGIC;
    index.generation = j.generation + 1;
    index.config = j.config;
    memcpy(index.head, j.head, sizeof(j.head));
    memcpy(index.count, j.count, sizeof(j.count));
    memcpy(index.hasFirst, j.hasFirst, sizeof(j.hasFirst));
    memcpy(index.firstAcked, j.firstAcked, sizeof(j.firstAcked));
    memcpy(index.dropped, j.dropped, sizeof(j.dropped));
    index.missedCuts = j.missedCuts;
    index.cutAt = j.cutAt;
    index.powerKnown = j.powerKnown;
    index.onBattery = j.onBattery;
    index.capturePending = j.capturePending;
    memcpy(index.sensorKnown, j.sensorKnown, sizeof(j.sensorKnown));
    memcpy(index.sensorState, j.sensorState, sizeof(j.sensorState));
  }

  bool load(int indexSlot, QueueIndex& index, Journal& j) {
    const char* indexName = indexSlot ? "index1" : "index0";
    if (prefs.getBytesLength(indexName) != sizeof(index) ||
        prefs.getBytes(indexName, &index, sizeof(index)) != sizeof(index) ||
        !validIndex(index)) {
      return false;
    }

    restoreMetadata(index, j);
    if (!validConfig(j.config)) {
      return false;
    }
    for (int sensor = 0; sensor < 2; ++sensor) {
      if (j.count[sensor] > MESH_FIFO_CAP || j.head[sensor] >= MESH_FIFO_CAP) {
        return false;
      }
      for (size_t position = 0; position < RECORDS_PER_SENSOR; ++position) {
        const bool first = position == MESH_FIFO_CAP;
        if (first ? !j.hasFirst[sensor] : !activeSlot(j, sensor, position)) {
          continue;
        }

        const size_t recordId = sensor * RECORDS_PER_SENSOR + position;
        char recordName[12];
        key(recordName, recordId, (index.banks >> recordId) & 1);
        StoredRecord record = {};
        if (prefs.getBytesLength(recordName) != sizeof(record) ||
            prefs.getBytes(recordName, &record, sizeof(record)) != sizeof(record)) {
          return false;
        }
        if (record.crc != index.recordCrc[recordId] ||
            record.crc != crc32(reinterpret_cast<const uint8_t*>(&record.sample), sizeof(Muestra)) ||
            !validSample(record.sample) || record.sample.sensor != sensor) {
          return false;
        }

        if (first) {
          j.first[sensor] = record.sample;
        } else {
          j.fifo[sensor][position] = record.sample;
        }
      }
    }
    return validJournal(j);
  }

public:
  bool begin(Journal& j) {
    if (!prefs.begin("iciv_hfr", false)) {
      return false;
    }
    const bool firstValid = load(0, active, j);
    const bool secondValid = load(1, candidate, previous);
    if (firstValid || secondValid) {
      slot = secondValid && (!firstValid || int32_t(candidate.generation - active.generation) > 0);
      if (slot) {
        active = candidate;
        j = previous;
      }
      previous = j;
      return true;
    }
    if (prefs.isKey("index0") || prefs.isKey("index1")) {
      return false;
    }

    journalInit(j);
    previous = j;
    return save(j);
  }

  bool save(Journal& j) {
    if (!validJournal(j)) {
      return false;
    }
    candidate = active;
    storeMetadata(j, candidate);

    for (int sensor = 0; sensor < 2; ++sensor) {
      for (size_t position = 0; position < RECORDS_PER_SENSOR; ++position) {
        const bool first = position == MESH_FIFO_CAP;
        if (first ? !j.hasFirst[sensor] : !activeSlot(j, sensor, position)) {
          continue;
        }

        const Muestra& sample = first ? j.first[sensor] : j.fifo[sensor][position];
        const Muestra& oldSample = first ? previous.first[sensor] : previous.fifo[sensor][position];
        const bool wasActive = first ? previous.hasFirst[sensor] : activeSlot(previous, sensor, position);
        if (wasActive && !memcmp(&sample, &oldSample, sizeof(sample))) {
          continue;
        }

        const size_t recordId = sensor * RECORDS_PER_SENSOR + position;
        const bool bank = !((active.banks >> recordId) & 1);
        char recordName[12];
        key(recordName, recordId, bank);
        StoredRecord record = {
          sample,
          crc32(reinterpret_cast<const uint8_t*>(&sample), sizeof(sample))
        };
        if (prefs.putBytes(recordName, &record, sizeof(record)) != sizeof(record)) {
          return false;
        }
        StoredRecord check = {};
        if (prefs.getBytes(recordName, &check, sizeof(check)) != sizeof(check) ||
            memcmp(&record, &check, sizeof(record))) {
          return false;
        }
        candidate.banks = (candidate.banks & ~(uint64_t(1) << recordId)) | (uint64_t(bank) << recordId);
        candidate.recordCrc[recordId] = record.crc;
      }
    }

    candidate.crc = crc32(reinterpret_cast<const uint8_t*>(&candidate), offsetof(QueueIndex, crc));
    const char* indexName = slot ? "index0" : "index1";
    if (prefs.putBytes(indexName, &candidate, sizeof(candidate)) != sizeof(candidate)) {
      return false;
    }
    QueueIndex check = {};
    if (prefs.getBytes(indexName, &check, sizeof(check)) != sizeof(check) ||
        memcmp(&candidate, &check, sizeof(check))) {
      return false;
    }
    slot = 1 - slot;
    active = candidate;
    j.generation = candidate.generation;
    previous = j;
    return true;
  }
};
#endif
