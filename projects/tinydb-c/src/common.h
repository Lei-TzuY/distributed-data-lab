#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#ifndef COMMON_H
#define COMMON_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
typedef SRWLOCK db_rwlock_t;
#define db_rwlock_init(lock) InitializeSRWLock(lock)
#define db_rwlock_rdlock(lock) AcquireSRWLockShared(lock)
#define db_rwlock_rdunlock(lock) ReleaseSRWLockShared(lock)
#define db_rwlock_wrlock(lock) AcquireSRWLockExclusive(lock)
#define db_rwlock_wrunlock(lock) ReleaseSRWLockExclusive(lock)
#define db_rwlock_destroy(lock) ((void)0)
#else
#include <pthread.h>
typedef pthread_rwlock_t db_rwlock_t;
#define db_rwlock_init(lock) pthread_rwlock_init(lock, NULL)
#define db_rwlock_rdlock(lock) pthread_rwlock_rdlock(lock)
#define db_rwlock_rdunlock(lock) pthread_rwlock_unlock(lock)
#define db_rwlock_wrlock(lock) pthread_rwlock_wrlock(lock)
#define db_rwlock_wrunlock(lock) pthread_rwlock_unlock(lock)
#define db_rwlock_destroy(lock) pthread_rwlock_destroy(lock)
#endif

#endif // COMMON_H
