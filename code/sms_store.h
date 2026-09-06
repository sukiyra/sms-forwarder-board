#ifndef SMS_STORE_H
#define SMS_STORE_H

#include <Arduino.h>

// Mounts (and, on first use, initializes) the persistent SMS store.
bool smsStoreBegin();

// Adds an unread message. Returns a non-zero monotonically increasing id on
// success, or 0 when the record could not be persisted.
uint32_t smsStoreAdd(const char* sender,
                     const char* body,
                     const char* timestamp,
                     const char* profile,
                     bool complete);

// Returns all non-deleted records, newest first. The JSON schema is:
// [{"id":1,"sender":"...","receiver":"...","body":"...",
//   "timestamp":"...","profile":"...","read":false,"complete":true}]
// `receiver` is the explicit semantic name; `profile` remains as a backwards-
// compatible alias for existing clients.
String smsStoreListJson();

bool smsStoreMarkRead(uint32_t id);
bool smsStoreDelete(uint32_t id);
bool smsStoreClear();

size_t smsStoreCount();
size_t smsStoreUnread();

#endif
