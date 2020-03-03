#pragma once

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <ws2def.h>
#include <ws2tcpip.h>
#define INVALID_TCP_SOCKET INVALID_SOCKET
typedef SOCKET TCP_SOCKET;

#else

#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>
#include <netdb.h>
#include <pthread.h> // compile with -pthread
#define INVALID_TCP_SOCKET (-1)
typedef int TCP_SOCKET;

#endif

struct THREAD_HANDLE;
struct RWLOCK_HANDLE;
struct MUTEX_HANDLE;

int TCP_INIT();
int TCP_CLOSE(TCP_SOCKET s);
int TCP_CLEANUP();
int TCP_WRITE(TCP_SOCKET s, const uint8_t* buf, int size);
int TCP_READ(TCP_SOCKET s, uint8_t* buf, int size);

int HTTP_READ(TCP_SOCKET s, int(*cb)(const char* header, const char* value, void* param), void* param, char body_overread[2048]);

int WS_WRITE(TCP_SOCKET s, const uint8_t* buf, int size, int split, bool bin);
int WS_READ(TCP_SOCKET s, uint8_t* buf, int size, bool* bin);

THREAD_HANDLE* THREAD_CREATE(void* (*entry)(void*), void* arg);
void* THREAD_JOIN(THREAD_HANDLE* thread);

bool THREAD_CREATE_DETACHED(void* (*entry)(void*), void* arg);

MUTEX_HANDLE* MUTEX_CREATE();
void MUTEX_DELETE(MUTEX_HANDLE* mutex);
void MUTEX_LOCK(MUTEX_HANDLE* mutex);
void MUTEX_UNLOCK(MUTEX_HANDLE* mutex);

RWLOCK_HANDLE* RWLOCK_CREATE();
void RWLOCK_DELETE(RWLOCK_HANDLE* rwl);
void RWLOCK_READ_LOCK(RWLOCK_HANDLE* rwl);
void RWLOCK_READ_UNLOCK(RWLOCK_HANDLE* rwl);
void RWLOCK_WRITE_LOCK(RWLOCK_HANDLE* rwl);
void RWLOCK_WRITE_UNLOCK(RWLOCK_HANDLE* rwl);

unsigned int INTERLOCKED_DEC(volatile unsigned int* ptr);
unsigned int INTERLOCKED_INC(volatile unsigned int* ptr);

////////////////////////////////////////////////////////////

struct STRUCT_REQ_JOIN
{
	uint8_t token; // 'J'
	char name[31];
};

struct STRUCT_RSP_JOIN
{
	uint8_t token; // 'j'
	uint8_t maxcli;	
	uint16_t id;
};

struct STRUCT_BRC_JOIN
{
	uint8_t token; // 'j' -- (theres collision with STRUCT_RSP_JOIN, but RSP is sent in sync, only once prior to any broadcast)
	uint8_t pad;
	uint16_t id;
	char name[32];
};

struct STRUCT_BRC_EXIT
{
	uint8_t token; // 'e'
	uint8_t pad;
	uint16_t id;
};

struct STRUCT_REQ_POSE
{
	uint8_t token; // 'P'
	uint8_t anim;
	uint8_t frame;
	uint8_t am; // action / mount
	float pos[3];
	float dir;
	uint16_t sprite;
};

struct STRUCT_BRC_POSE
{
	uint8_t token; // 'p'
	uint8_t anim;
	uint8_t frame;
	uint8_t am; // action / mount
	float pos[3];
	float dir;
	uint16_t sprite;
	uint16_t id;
};


