// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef PAGANI_S3910_H
#define PAGANI_S3910_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define S3910_CONTACTS 10
#define S3910_PAYLOAD_MAX 512
struct s3910_contact { uint16_t x, y; uint8_t active; };
struct s3910_frame { struct s3910_contact contact[S3910_CONTACTS]; };
struct s3910_bus {
    void *context;
    int (*read)(void *, uint8_t *, size_t);
    int (*write)(void *, const uint8_t *, size_t);
    void (*sleep_ms)(void *, unsigned);
};
struct s3910 {
    struct s3910_bus bus;
    uint16_t max_x, max_y, config_size, max_write, max_read;
    uint8_t mode, code, ready, crc, version, sequence;
    uint16_t length;
    uint8_t command;
    const char *stage;
    uint32_t detail;
    uint8_t payload[S3910_PAYLOAD_MAX];
};
// The S3910 firmware remains on the controller; this path never erases or flashes it.
int s3910_start(struct s3910 *);
// 1 = complete frame, 0 = other report/idle, negative = protocol/transport failure.
int s3910_poll(struct s3910 *, struct s3910_frame *);
int s3910_decode(const uint8_t *, size_t, uint16_t, uint16_t, struct s3910_frame *);
void s3910_hid_report(const struct s3910_frame *, const struct s3910_frame *, uint8_t report[62]);
#ifdef __cplusplus
}
#endif
#endif
