#ifndef FACTORY_SERIAL_H
#define FACTORY_SERIAL_H

// Handles the read-only USB factory protocol and preserves the existing AT
// passthrough for ordinary serial terminals.
void factorySerialLoop();

#endif
