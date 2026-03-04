#pragma once
#include <liburing.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdarg.h>

// Inicjalizacja ringu io_uring
static inline int log_uring_init(struct io_uring* ring, unsigned depth) {
    return io_uring_queue_init(depth, ring, 0);
}

// Wewnetrzna funkcja: submituje asynchroniczny zapis do fd.
// buf musi byc heap-allocated – zostanie zwolniony po zakonczeniu zapisu.
static inline void log_uring_submit(struct io_uring* ring, int fd,
                                     char* buf, int len) {
    struct io_uring_sqe* sqe = io_uring_get_sqe(ring);
    if (!sqe) {
        // Ring pelny – synchroniczny fallback
        [[maybe_unused]] ssize_t _wr = write(fd, buf, len);
        free(buf);
        return;
    }
    // offset = (uint64_t)-1 oznacza "uzyj aktualnej pozycji pliku" (jak write())
    // Wymagane przy O_APPEND zeby wszystkie procesy dodawaly na koniec
    io_uring_prep_write(sqe, fd, buf, (unsigned)len, (uint64_t)-1);
    io_uring_sqe_set_data(sqe, buf);   // user_data = wskaznik do bufora (do free)
    io_uring_submit(ring);

    // Zwolnij bufory juz zakonczonych operacji (non-blocking peek)
    struct io_uring_cqe* cqe;
    while (io_uring_peek_cqe(ring, &cqe) == 0) {
        void* udata = io_uring_cqe_get_data(cqe);
        if (udata) free(udata);
        io_uring_cqe_seen(ring, cqe);
    }
}

// Formatuje wiadomosc i submituje asynchroniczny zapis (io_uring IORING_OP_WRITE)
static inline void log_uring_printf(struct io_uring* ring, int fd,
                                     const char* fmt, va_list ap) {
    char tmp[1024];
    int len = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    if (len <= 0) return;

    char* buf = (char*)malloc(len + 1);
    if (!buf) {
        // Brak pamieci – synchroniczny fallback
        [[maybe_unused]] ssize_t _wr = write(fd, tmp, len);
        return;
    }
    memcpy(buf, tmp, len);
    log_uring_submit(ring, fd, buf, len);
}

// Flush: czeka az wszystkie in-flight zapisy sie ukonczą (wywolaj przed exit)
static inline void log_uring_flush(struct io_uring* ring) {
    io_uring_submit(ring);
    struct io_uring_cqe* cqe;
    struct __kernel_timespec ts = { 0, 100 * 1000000L };  // 100ms timeout
    // Petla: czeka na kolejne CQE az do timeoutu (brak operacji = -ETIME)
    while (io_uring_wait_cqe_timeout(ring, &cqe, &ts) == 0) {
        void* udata = io_uring_cqe_get_data(cqe);
        if (udata) free(udata);
        io_uring_cqe_seen(ring, cqe);
    }
}

// Zniszczenie ringu
static inline void log_uring_destroy(struct io_uring* ring) {
    io_uring_queue_exit(ring);
}
