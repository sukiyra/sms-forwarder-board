#include "sms_store.h"

#include <LittleFS.h>
#include <stddef.h>
#include <string.h>

namespace {

constexpr char kStorePath[] = "/sms_store.bin";
constexpr uint32_t kHeaderMagic = 0x534D5348UL;  // "SMSH"
constexpr uint32_t kRecordMagic = 0x534D5352UL;  // "SMSR"
constexpr uint16_t kStoreVersion = 1;
constexpr uint16_t kCapacity = 50;

constexpr uint8_t kFlagValid = 1U << 0;
constexpr uint8_t kFlagRead = 1U << 1;
constexpr uint8_t kFlagDeleted = 1U << 2;
constexpr uint8_t kFlagComplete = 1U << 3;

struct __attribute__((packed)) StoreHeader {
  uint32_t magic;
  uint16_t version;
  uint16_t recordSize;
  uint16_t capacity;
  uint16_t writeIndex;
  uint32_t nextId;
  uint32_t crc32;
};

struct __attribute__((packed)) StoredSms {
  uint32_t magic;
  uint32_t id;
  uint8_t flags;
  uint8_t reserved[3];
  char sender[33];       // 32 UTF-8 bytes + NUL
  char timestamp[25];    // 24 UTF-8 bytes + NUL
  char profile[49];      // 48 UTF-8 bytes + NUL
  char body[641];        // 640 UTF-8 bytes + NUL
  uint32_t crc32;
};

struct RecordRef {
  uint32_t id;
  uint16_t slot;
};

constexpr size_t kExpectedFileSize =
    sizeof(StoreHeader) + static_cast<size_t>(kCapacity) * sizeof(StoredSms);

StoreHeader storeHeader = {};
bool storeReady = false;
size_t activeCount = 0;
size_t unreadCount = 0;

uint32_t crc32Of(const uint8_t* data, size_t length) {
  uint32_t crc = 0xFFFFFFFFUL;
  while (length-- > 0) {
    crc ^= *data++;
    for (uint8_t bit = 0; bit < 8; ++bit) {
      const uint32_t mask = 0U - (crc & 1U);
      crc = (crc >> 1) ^ (0xEDB88320UL & mask);
    }
  }
  return ~crc;
}

uint32_t headerCrc(const StoreHeader& header) {
  return crc32Of(reinterpret_cast<const uint8_t*>(&header),
                 offsetof(StoreHeader, crc32));
}

uint32_t recordCrc(const StoredSms& record) {
  return crc32Of(reinterpret_cast<const uint8_t*>(&record),
                 offsetof(StoredSms, crc32));
}

bool isHeaderValid(const StoreHeader& header) {
  return header.magic == kHeaderMagic &&
         header.version == kStoreVersion &&
         header.recordSize == sizeof(StoredSms) &&
         header.capacity == kCapacity &&
         header.writeIndex < kCapacity &&
         header.nextId != 0 &&
         header.crc32 == headerCrc(header);
}

bool isRecordValid(const StoredSms& record) {
  return record.magic == kRecordMagic &&
         record.id != 0 &&
         (record.flags & kFlagValid) != 0 &&
         record.crc32 == recordCrc(record);
}

size_t recordOffset(uint16_t slot) {
  return sizeof(StoreHeader) + static_cast<size_t>(slot) * sizeof(StoredSms);
}

bool readHeader(File& file, StoreHeader& header) {
  return file.seek(0, SeekSet) &&
         file.read(reinterpret_cast<uint8_t*>(&header), sizeof(header)) ==
             sizeof(header);
}

bool writeHeader(File& file, StoreHeader& header) {
  header.crc32 = headerCrc(header);
  if (!file.seek(0, SeekSet)) return false;
  if (file.write(reinterpret_cast<const uint8_t*>(&header), sizeof(header)) !=
      sizeof(header)) {
    return false;
  }
  file.flush();
  return true;
}

bool readRecord(File& file, uint16_t slot, StoredSms& record) {
  if (slot >= kCapacity || !file.seek(recordOffset(slot), SeekSet)) return false;
  if (file.read(reinterpret_cast<uint8_t*>(&record), sizeof(record)) !=
      sizeof(record)) {
    return false;
  }
  return isRecordValid(record);
}

bool writeRecord(File& file, uint16_t slot, StoredSms& record) {
  if (slot >= kCapacity || !file.seek(recordOffset(slot), SeekSet)) return false;
  record.crc32 = recordCrc(record);
  if (file.write(reinterpret_cast<const uint8_t*>(&record), sizeof(record)) !=
      sizeof(record)) {
    return false;
  }
  file.flush();
  return true;
}

StoreHeader makeHeader(uint32_t nextId) {
  StoreHeader header = {};
  header.magic = kHeaderMagic;
  header.version = kStoreVersion;
  header.recordSize = sizeof(StoredSms);
  header.capacity = kCapacity;
  header.writeIndex = 0;
  header.nextId = nextId == 0 ? 1 : nextId;
  header.crc32 = headerCrc(header);
  return header;
}

bool createFreshStore(uint32_t nextId) {
  File file = LittleFS.open(kStorePath, "w");
  if (!file) return false;

  StoreHeader header = makeHeader(nextId);
  if (!writeHeader(file, header)) {
    file.close();
    return false;
  }

  const StoredSms emptyRecord = {};
  for (uint16_t slot = 0; slot < kCapacity; ++slot) {
    if (file.write(reinterpret_cast<const uint8_t*>(&emptyRecord),
                   sizeof(emptyRecord)) != sizeof(emptyRecord)) {
      file.close();
      return false;
    }
  }
  file.flush();
  const bool sizeOk = file.size() == kExpectedFileSize;
  file.close();

  if (!sizeOk) return false;
  storeHeader = header;
  activeCount = 0;
  unreadCount = 0;
  return true;
}

bool idIsNewer(uint32_t candidate, uint32_t reference) {
  return static_cast<int32_t>(candidate - reference) > 0;
}

uint32_t nextNonZeroId(uint32_t id) {
  ++id;
  return id == 0 ? 1 : id;
}

void terminateRecordStrings(StoredSms& record) {
  record.sender[sizeof(record.sender) - 1] = '\0';
  record.timestamp[sizeof(record.timestamp) - 1] = '\0';
  record.profile[sizeof(record.profile) - 1] = '\0';
  record.body[sizeof(record.body) - 1] = '\0';
}

bool isContinuation(uint8_t value) {
  return (value & 0xC0U) == 0x80U;
}

// Returns a valid UTF-8 sequence length, or zero for an invalid leading byte
// or sequence. Checks for overlong encodings, surrogates and values > U+10FFFF.
size_t validUtf8Length(const uint8_t* source) {
  const uint8_t first = source[0];
  if (first < 0x80U) return 1;

  const uint8_t second = source[1];
  if (second == 0) return 0;

  if (first >= 0xC2U && first <= 0xDFU) {
    return isContinuation(second) ? 2 : 0;
  }

  const uint8_t third = source[2];
  if (third == 0) return 0;
  if (first == 0xE0U) {
    return second >= 0xA0U && second <= 0xBFU && isContinuation(third) ? 3 : 0;
  }
  if ((first >= 0xE1U && first <= 0xECU) ||
      (first >= 0xEEU && first <= 0xEFU)) {
    return isContinuation(second) && isContinuation(third) ? 3 : 0;
  }
  if (first == 0xEDU) {
    return second >= 0x80U && second <= 0x9FU && isContinuation(third) ? 3 : 0;
  }

  const uint8_t fourth = source[3];
  if (fourth == 0) return 0;
  if (first == 0xF0U) {
    return second >= 0x90U && second <= 0xBFU && isContinuation(third) &&
                   isContinuation(fourth)
               ? 4
               : 0;
  }
  if (first >= 0xF1U && first <= 0xF3U) {
    return isContinuation(second) && isContinuation(third) &&
                   isContinuation(fourth)
               ? 4
               : 0;
  }
  if (first == 0xF4U) {
    return second >= 0x80U && second <= 0x8FU && isContinuation(third) &&
                   isContinuation(fourth)
               ? 4
               : 0;
  }
  return 0;
}

bool copyUtf8Truncated(char* destination,
                       size_t destinationSize,
                       const char* source) {
  if (destinationSize == 0) return source == nullptr || *source == '\0';
  destination[0] = '\0';
  if (source == nullptr) return true;

  const size_t maxBytes = destinationSize - 1;
  size_t written = 0;
  const uint8_t* cursor = reinterpret_cast<const uint8_t*>(source);

  while (*cursor != 0) {
    size_t sequenceLength = validUtf8Length(cursor);
    if (sequenceLength == 0) {
      if (written >= maxBytes) break;
      destination[written++] = '?';
      ++cursor;
      continue;
    }
    if (written + sequenceLength > maxBytes) break;
    memcpy(destination + written, cursor, sequenceLength);
    written += sequenceLength;
    cursor += sequenceLength;
  }
  destination[written] = '\0';
  return *cursor == 0;
}

bool findRecord(File& file, uint32_t id, uint16_t& slotOut, StoredSms& recordOut) {
  if (id == 0) return false;
  for (uint16_t slot = 0; slot < kCapacity; ++slot) {
    StoredSms record = {};
    if (readRecord(file, slot, record) && record.id == id) {
      terminateRecordStrings(record);
      slotOut = slot;
      recordOut = record;
      return true;
    }
  }
  return false;
}

size_t decimalLength(uint32_t value) {
  size_t length = 1;
  while (value >= 10) {
    value /= 10;
    ++length;
  }
  return length;
}

size_t jsonEscapedLength(const char* text) {
  size_t length = 0;
  for (const uint8_t* cursor = reinterpret_cast<const uint8_t*>(text);
       *cursor != 0; ++cursor) {
    switch (*cursor) {
      case '"':
      case '\\':
      case '\b':
      case '\f':
      case '\n':
      case '\r':
      case '\t':
        length += 2;
        break;
      default:
        length += *cursor < 0x20U ? 6 : 1;
        break;
    }
  }
  return length;
}

void appendJsonEscaped(String& json, const char* text) {
  static constexpr char hex[] = "0123456789ABCDEF";
  for (const uint8_t* cursor = reinterpret_cast<const uint8_t*>(text);
       *cursor != 0; ++cursor) {
    switch (*cursor) {
      case '"': json += F("\\\""); break;
      case '\\': json += F("\\\\"); break;
      case '\b': json += F("\\b"); break;
      case '\f': json += F("\\f"); break;
      case '\n': json += F("\\n"); break;
      case '\r': json += F("\\r"); break;
      case '\t': json += F("\\t"); break;
      default:
        if (*cursor < 0x20U) {
          json += F("\\u00");
          json += hex[(*cursor >> 4) & 0x0FU];
          json += hex[*cursor & 0x0FU];
        } else {
          json += static_cast<char>(*cursor);
        }
        break;
    }
  }
}

size_t recordJsonLength(const StoredSms& record) {
  size_t length = sizeof("{\"id\":") - 1 + decimalLength(record.id);
  length += sizeof(",\"sender\":\"") - 1 + jsonEscapedLength(record.sender) + 1;
  length += sizeof(",\"receiver\":\"") - 1 + jsonEscapedLength(record.profile) + 1;
  length += sizeof(",\"body\":\"") - 1 + jsonEscapedLength(record.body) + 1;
  length += sizeof(",\"timestamp\":\"") - 1 +
            jsonEscapedLength(record.timestamp) + 1;
  length += sizeof(",\"profile\":\"") - 1 + jsonEscapedLength(record.profile) + 1;
  length += sizeof(",\"read\":") - 1 +
            ((record.flags & kFlagRead) != 0 ? 4 : 5);
  length += sizeof(",\"complete\":") - 1 +
            ((record.flags & kFlagComplete) != 0 ? 4 : 5);
  return length + 1;  // closing object brace
}

void appendRecordJson(String& json, const StoredSms& record) {
  json += F("{\"id\":");
  json += record.id;
  json += F(",\"sender\":\"");
  appendJsonEscaped(json, record.sender);
  json += F("\",\"receiver\":\"");
  appendJsonEscaped(json, record.profile);
  json += F("\",\"body\":\"");
  appendJsonEscaped(json, record.body);
  json += F("\",\"timestamp\":\"");
  appendJsonEscaped(json, record.timestamp);
  json += F("\",\"profile\":\"");
  appendJsonEscaped(json, record.profile);
  json += F("\",\"read\":");
  json += (record.flags & kFlagRead) != 0 ? F("true") : F("false");
  json += F(",\"complete\":");
  json += (record.flags & kFlagComplete) != 0 ? F("true") : F("false");
  json += '}';
}

}  // namespace

bool smsStoreBegin() {
  if (storeReady) return true;

  if (!LittleFS.begin(false) && !LittleFS.begin(true)) return false;

  if (!LittleFS.exists(kStorePath)) {
    storeReady = createFreshStore(1);
    return storeReady;
  }

  File file = LittleFS.open(kStorePath, "r+");
  if (!file) return false;
  if (file.size() != kExpectedFileSize) {
    file.close();
    storeReady = createFreshStore(1);
    return storeReady;
  }

  StoreHeader diskHeader = {};
  const bool headerWasValid = readHeader(file, diskHeader) && isHeaderValid(diskHeader);
  StoreHeader rebuiltHeader = headerWasValid ? diskHeader : makeHeader(1);

  activeCount = 0;
  unreadCount = 0;
  bool haveLatest = false;
  uint32_t latestId = 0;
  uint16_t latestSlot = 0;

  for (uint16_t slot = 0; slot < kCapacity; ++slot) {
    StoredSms record = {};
    if (!readRecord(file, slot, record)) continue;

    if (!haveLatest || idIsNewer(record.id, latestId)) {
      haveLatest = true;
      latestId = record.id;
      latestSlot = slot;
    }
    if ((record.flags & kFlagDeleted) == 0) {
      ++activeCount;
      if ((record.flags & kFlagRead) == 0) ++unreadCount;
    }
  }

  if (haveLatest) {
    rebuiltHeader.nextId = nextNonZeroId(latestId);
    rebuiltHeader.writeIndex = (latestSlot + 1U) % kCapacity;
  } else if (!headerWasValid) {
    rebuiltHeader = makeHeader(1);
  }

  const bool repaired = writeHeader(file, rebuiltHeader);
  file.close();
  if (!repaired) return false;

  storeHeader = rebuiltHeader;
  storeReady = true;
  return true;
}

uint32_t smsStoreAdd(const char* sender,
                     const char* body,
                     const char* timestamp,
                     const char* profile,
                     bool complete) {
  if (!storeReady) return 0;

  File file = LittleFS.open(kStorePath, "r+");
  if (!file) return 0;

  const uint16_t slot = storeHeader.writeIndex;
  StoredSms replaced = {};
  const bool replacedValid = readRecord(file, slot, replaced);

  StoredSms record = {};
  record.magic = kRecordMagic;
  record.id = storeHeader.nextId;
  record.flags = kFlagValid | (complete ? kFlagComplete : 0);
  copyUtf8Truncated(record.sender, sizeof(record.sender), sender);
  if (!copyUtf8Truncated(record.body, sizeof(record.body), body)) {
    record.flags &= ~kFlagComplete;
  }
  copyUtf8Truncated(record.timestamp, sizeof(record.timestamp), timestamp);
  copyUtf8Truncated(record.profile, sizeof(record.profile), profile);

  if (!writeRecord(file, slot, record)) {
    file.close();
    return 0;
  }

  if (replacedValid && (replaced.flags & kFlagDeleted) == 0) {
    if (activeCount > 0) --activeCount;
    if ((replaced.flags & kFlagRead) == 0 && unreadCount > 0) --unreadCount;
  }
  ++activeCount;
  ++unreadCount;

  storeHeader.writeIndex = (slot + 1U) % kCapacity;
  storeHeader.nextId = nextNonZeroId(record.id);
  // The record is already durable. If the header update is interrupted, begin()
  // reconstructs both fields from the newest valid record.
  writeHeader(file, storeHeader);
  file.close();
  return record.id;
}

String smsStoreListJson() {
  if (!storeReady) return String(F("[]"));

  File file = LittleFS.open(kStorePath, "r");
  if (!file) return String(F("[]"));

  RecordRef refs[kCapacity];
  size_t refCount = 0;
  for (uint16_t slot = 0; slot < kCapacity; ++slot) {
    StoredSms record = {};
    if (readRecord(file, slot, record) &&
        (record.flags & kFlagDeleted) == 0) {
      refs[refCount++] = {record.id, slot};
    }
  }

  // Insertion sort is compact, and the list never exceeds 50 entries.
  for (size_t i = 1; i < refCount; ++i) {
    const RecordRef key = refs[i];
    size_t j = i;
    while (j > 0 && idIsNewer(key.id, refs[j - 1].id)) {
      refs[j] = refs[j - 1];
      --j;
    }
    refs[j] = key;
  }

  size_t requiredLength = 2 + (refCount > 0 ? refCount - 1 : 0);
  for (size_t i = 0; i < refCount; ++i) {
    StoredSms record = {};
    if (!readRecord(file, refs[i].slot, record)) {
      file.close();
      return String(F("[]"));
    }
    terminateRecordStrings(record);
    requiredLength += recordJsonLength(record);
  }

  String json;
  if (!json.reserve(requiredLength)) {
    file.close();
    return String(F("[]"));
  }
  json += '[';
  for (size_t i = 0; i < refCount; ++i) {
    StoredSms record = {};
    if (!readRecord(file, refs[i].slot, record)) {
      file.close();
      return String(F("[]"));
    }
    terminateRecordStrings(record);
    if (i > 0) json += ',';
    appendRecordJson(json, record);
  }
  json += ']';
  file.close();

  return json.length() == requiredLength ? json : String(F("[]"));
}

bool smsStoreMarkRead(uint32_t id) {
  if (!storeReady || id == 0) return false;

  File file = LittleFS.open(kStorePath, "r+");
  if (!file) return false;

  uint16_t slot = 0;
  StoredSms record = {};
  if (!findRecord(file, id, slot, record) ||
      (record.flags & kFlagDeleted) != 0) {
    file.close();
    return false;
  }
  if ((record.flags & kFlagRead) != 0) {
    file.close();
    return true;
  }

  record.flags |= kFlagRead;
  const bool written = writeRecord(file, slot, record);
  file.close();
  if (written && unreadCount > 0) --unreadCount;
  return written;
}

bool smsStoreDelete(uint32_t id) {
  if (!storeReady || id == 0) return false;

  File file = LittleFS.open(kStorePath, "r+");
  if (!file) return false;

  uint16_t slot = 0;
  StoredSms record = {};
  if (!findRecord(file, id, slot, record)) {
    file.close();
    return false;
  }
  if ((record.flags & kFlagDeleted) != 0) {
    file.close();
    return true;
  }

  const bool wasUnread = (record.flags & kFlagRead) == 0;
  record.flags |= kFlagDeleted;
  const bool written = writeRecord(file, slot, record);
  file.close();
  if (written) {
    if (activeCount > 0) --activeCount;
    if (wasUnread && unreadCount > 0) --unreadCount;
  }
  return written;
}

bool smsStoreClear() {
  if (!storeReady) return false;
  const uint32_t nextId = storeHeader.nextId;
  storeReady = false;
  storeReady = createFreshStore(nextId);
  return storeReady;
}

size_t smsStoreCount() {
  return storeReady ? activeCount : 0;
}

size_t smsStoreUnread() {
  return storeReady ? unreadCount : 0;
}
