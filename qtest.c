/*
 * Test Server
 *
 * Copyright IBM, Corp. 2011
 *
 * Authors:
 *  Anthony Liguori   <aliguori@us.ibm.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 */

#include "sysemu/qtest.h"
#include "hw/qdev.h"
#include "char/char.h"
#include "exec/ioport.h"
#include "exec/memory.h"
#include "hw/irq.h"
#include "sysemu/sysemu.h"
#include "sysemu/cpus.h"

#define MAX_IRQ 256

const char *qtest_chrdev;
const char *qtest_log;
bool qtest_allowed;

static DeviceState *irq_intercept_dev;
static FILE *qtest_log_fp;
static CharDriverState *qtest_chr;
static GString *inbuf;
static int irq_levels[MAX_IRQ];
static qemu_timeval start_time;
static bool qtest_opened;

#define FMT_timeval "%ld.%06ld"

/**
 * QTest Protocol
 *
 * Line based protocol, request/response based.  Server can send async messages
 * so clients should always handle many async messages before the response
 * comes in.
 *
 * Valid requests
 *
 * Clock management:
 *
 * The qtest client is completely in charge of the vm_clock.  qtest commands
 * let you adjust the value of the clock (monotonically).  All the commands
 * return the current value of the clock in nanoseconds.
 *
 *  > clock_step
 *  < OK VALUE
 *
 *     Advance the clock to the next deadline.  Useful when waiting for
 *     asynchronous events.
 *
 *  > clock_step NS
 *  < OK VALUE
 *
 *     Advance the clock by NS nanoseconds.
 *
 *  > clock_set NS
 *  < OK VALUE
 *
 *     Advance the clock to NS nanoseconds (do nothing if it's already past).
 *
 * PIO and memory access:
 *
 *  > outb ADDR VALUE
 *  < OK
 *
 *  > outw ADDR VALUE
 *  < OK
 *
 *  > outl ADDR VALUE
 *  < OK
 *
 *  > inb ADDR
 *  < OK VALUE
 *
 *  > inw ADDR
 *  < OK VALUE
 *
 *  > inl ADDR
 *  < OK VALUE
 *
 *  > read ADDR SIZE
 *  < OK DATA
 *
 *  > write ADDR SIZE DATA
 *  < OK
 *
 * ADDR, SIZE, VALUE are all integers parsed with strtoul() with a base of 0.
 *
 * DATA is an arbitrarily long hex number prefixed with '0x'.  If it's smaller
 * than the expected size, the value will be zero filled at the end of the data
 * sequence.
 *
 * IRQ management:
 *
 *  > irq_intercept_in QOM-PATH
 *  < OK
 *
 *  > irq_intercept_out QOM-PATH
 *  < OK
 *
 * Attach to the gpio-in (resp. gpio-out) pins exported by the device at
 * QOM-PATH.  When the pin is triggered, one of the following async messages
 * will be printed to the qtest stream:
 *
 *  IRQ raise NUM
 *  IRQ lower NUM
 *
 * where NUM is an IRQ number.  For the PC, interrupts can be intercepted
 * simply with "irq_intercept_in ioapic" (note that IRQ0 comes out with
 * NUM=0 even though it is remapped to GSI 2).
 */

static int hex2nib(char ch)
{
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    } else if (ch >= 'a' && ch <= 'f') {
        return 10 + (ch - 'a');
    } else if (ch >= 'A' && ch <= 'F') {
        return 10 + (ch - 'a');
    } else {
        return -1;
    }
}

static void qtest_get_time(qemu_timeval *tv)
{
    qemu_gettimeofday(tv);
    tv->tv_sec -= start_time.tv_sec;
    tv->tv_usec -= start_time.tv_usec;
    if (tv->tv_usec < 0) {
        tv->tv_usec += 1000000;
        tv->tv_sec -= 1;
    }
}

static void qtest_send_prefix(CharDriverState *chr)
{
    qemu_timeval tv;

    if (!qtest_log_fp || !qtest_opened) {
        return;
    }

    qtest_get_time(&tv);
    fprintf(qtest_log_fp, "[S +" FMT_timeval "] ",
            tv.tv_sec, (long) tv.tv_usec);
}

static void GCC_FMT_ATTR(2, 3) qtest_send(CharDriverState *chr,
                                          const char *fmt, ...)
{
    va_list ap;
    char buffer[1024];
    size_t len;

    va_start(ap, fmt);
    len = vsnprintf(buffer, sizeof(buffer), fmt, ap);
    va_end(ap);

    qemu_chr_fe_write(chr, (uint8_t *)buffer, len);
    if (qtest_log_fp && qtest_opened) {
        fprintf(qtest_log_fp, "%s", buffer);
    }
}

static void qtest_irq_handler(void *opaque, int n, int level)
{
    qemu_irq *old_irqs = opaque;
    qemu_set_irq(old_irqs[n], level);

    if (irq_levels[n] != level) {
        CharDriverState *chr = qtest_chr;
        irq_levels[n] = level;
        qtest_send_prefix(chr);
        qtest_send(chr, "IRQ %s %d\n",
                   level ? "raise" : "lower", n);
    }
}

static void qtest_process_command(CharDriverState *chr, gchar **words)
{
    const gchar *command;

    g_assert(words);

    command = words[0];

    if (qtest_log_fp) {
        qemu_timeval tv;
        int i;

        qtest_get_time(&tv);
        fprintf(qtest_log_fp, "[R +" FMT_timeval "]",
                tv.tv_sec, (long) tv.tv_usec);
        for (i = 0; words[i]; i++) {
            fprintf(qtest_log_fp, " %s", words[i]);
        }
        fprintf(qtest_log_fp, "\n");
    }

    g_assert(command);
    if (strcmp(words[0], "irq_intercept_out") == 0
        || strcmp(words[0], "irq_intercept_in") == 0) {
	DeviceState *dev;

        g_assert(words[1]);
        dev = DEVICE(object_resolve_path(words[1], NULL));
        if (!dev) {
            qtest_send_prefix(chr);
            qtest_send(chr, "FAIL Unknown device\n");
	    return;
        }

        if (irq_intercept_dev) {
            qtest_send_prefix(chr);
            if (irq_intercept_dev != dev) {
                qtest_send(chr, "FAIL IRQ intercept already enabled\n");
            } else {
                qtest_send(chr, "OK\n");
            }
	    return;
        }

        if (words[0][14] == 'o') {
            qemu_irq_intercept_out(&dev->gpio_out, qtest_irq_handler, dev->num_gpio_out);
        } else {
            qemu_irq_intercept_in(dev->gpio_in, qtest_irq_handler, dev->num_gpio_in);
        }
        irq_intercept_dev = dev;
        qtest_send_prefix(chr);
        qtest_send(chr, "OK\n");

    } else if (strcmp(words[0], "outb") == 0 ||
               strcmp(words[0], "outw") == 0 ||
               strcmp(words[0], "outl") == 0) {
        uint16_t addr;
        uint32_t value;

        g_assert(words[1] && words[2]);
        addr = strtol(words[1], NULL, 0);
        value = strtol(words[2], NULL, 0);

        if (words[0][3] == 'b') {
            cpu_outb(addr, value);
        } else if (words[0][3] == 'w') {
            cpu_outw(addr, value);
        } else if (words[0][3] == 'l') {
            cpu_outl(addr, value);
        }
        qtest_send_prefix(chr);
        qtest_send(chr, "OK\n");
    } else if (strcmp(words[0], "inb") == 0 ||
        strcmp(words[0], "inw") == 0 ||
        strcmp(words[0], "inl") == 0) {
        uint16_t addr;
        uint32_t value = -1U;

        g_assert(words[1]);
        addr = strtol(words[1], NULL, 0);

        if (words[0][2] == 'b') {
            value = cpu_inb(addr);
        } else if (words[0][2] == 'w') {
            value = cpu_inw(addr);
        } else if (words[0][2] == 'l') {
            value = cpu_inl(addr);
        }
        qtest_send_prefix(chr);
        qtest_send(chr, "OK 0x%04x\n", value);
    } else if (strcmp(words[0], "read") == 0) {
        uint64_t addr, len, i;
        uint8_t *data;

        g_assert(words[1] && words[2]);
        addr = strtoull(words[1], NULL, 0);
        len = strtoull(words[2], NULL, 0);

        data = g_malloc(len);
        cpu_physical_memory_read(addr, data, len);

        qtest_send_prefix(chr);
        qtest_send(chr, "OK 0x");
        for (i = 0; i < len; i++) {
            qtest_send(chr, "%02x", data[i]);
        }
        qtest_send(chr, "\n");

        g_free(data);
    } else if (strcmp(words[0], "write") == 0) {
        uint64_t addr, len, i;
        uint8_t *data;
        size_t data_len;

        g_assert(words[1] && words[2] && words[3]);
        addr = strtoull(words[1], NULL, 0);
        len = strtoull(words[2], NULL, 0);

        data_len = strlen(words[3]);
        if (data_len < 3) {
            qtest_send(chr, "ERR invalid argument size\n");
            return;
        }

        data = g_malloc(len);
        for (i = 0; i < len; i++) {
            if ((i * 2 + 4) <= data_len) {
                data[i] = hex2nib(words[3][i * 2 + 2]) << 4;
                data[i] |= hex2nib(words[3][i * 2 + 3]);
            } else {
                data[i] = 0;
            }
        }
        cpu_physical_memory_write(addr, data, len);
        g_free(data);

        qtest_send_prefix(chr);
        qtest_send(chr, "OK\n");
    } else if (strcmp(words[0], "clock_step") == 0) {
        int64_t ns;

        if (words[1]) {
            ns = strtoll(words[1], NULL, 0);
        } else {
            ns = qemu_clock_deadline(vm_clock);
        }
        qtest_clock_warp(qemu_get_clock_ns(vm_clock) + ns);
        qtest_send_prefix(chr);
        qtest_send(chr, "OK %"PRIi64"\n", (int64_t)qemu_get_clock_ns(vm_clock));
    } else if (strcmp(words[0], "clock_set") == 0) {
        int64_t ns;

        g_assert(words[1]);
        ns = strtoll(words[1], NULL, 0);
        qtest_clock_warp(ns);
        qtest_send_prefix(chr);
        qtest_send(chr, "OK %"PRIi64"\n", (int64_t)qemu_get_clock_ns(vm_clock));
    } else {
        qtest_send_prefix(chr);
        qtest_send(chr, "FAIL Unknown command `%s'\n", words[0]);
    }
}

static void qtest_process_inbuf(CharDriverState *chr, GString *inbuf)
{
    char *end;

    while ((end = strchr(inbuf->str, '\n')) != NULL) {
        size_t offset;
        GString *cmd;
        gchar **words;

        offset = end - inbuf->str;

        cmd = g_string_new_len(inbuf->str, offset);
        g_string_erase(inbuf, 0, offset + 1);

        words = g_strsplit(cmd->str, " ", 0);
        qtest_process_command(chr, words);
        g_strfreev(words);

        g_string_free(cmd, TRUE);
    }
}

static void qtest_read(void *opaque, const uint8_t *buf, int size)
{
    CharDriverState *chr = opaque;

    g_string_append_len(inbuf, (const gchar *)buf, size);
    qtest_process_inbuf(chr, inbuf);
}

static int qtest_can_read(void *opaque)
{
    return 1024;
}

static void qtest_event(void *opaque, int event)
{
    int i;

    switch (event) {
    case CHR_EVENT_OPENED:
        qemu_system_reset(false);
        for (i = 0; i < ARRAY_SIZE(irq_levels); i++) {
            irq_levels[i] = 0;
        }
        qemu_gettimeofday(&start_time);
        qtest_opened = true;
        if (qtest_log_fp) {
            fprintf(qtest_log_fp, "[I " FMT_timeval "] OPENED\n",
                    start_time.tv_sec, (long) start_time.tv_usec);
        }
        break;
    case CHR_EVENT_CLOSED:
        qtest_opened = false;
        if (qtest_log_fp) {
            qemu_timeval tv;
            qtest_get_time(&tv);
            fprintf(qtest_log_fp, "[I +" FMT_timeval "] CLOSED\n",
                    tv.tv_sec, (long) tv.tv_usec);
        }
        break;
    default:
        break;
    }
}

static const QemuChrHandlers test_handlers = {
    .fd_can_read = qtest_can_read,
    .fd_read = qtest_read,
    .fd_event = qtest_event,
};


int qtest_init(void)
{
    CharDriverState *chr;

    g_assert(qtest_chrdev != NULL);

    configure_icount("0");
    chr = qemu_chr_new("qtest", qtest_chrdev, NULL);

    qemu_chr_add_handlers(chr, &test_handlers, chr);
    qemu_chr_fe_set_echo(chr, true);

    inbuf = g_string_new("");

    if (qtest_log) {
        if (strcmp(qtest_log, "none") != 0) {
            qtest_log_fp = fopen(qtest_log, "w+");
        }
    } else {
        qtest_log_fp = stderr;
    }

    qtest_chr = chr;

    return 0;
}
