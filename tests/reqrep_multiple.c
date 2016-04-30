/*
    Copyright Franklin "Snaipe" Mathieu <franklinmathieu@gmail.com>

    Permission is hereby granted, free of charge, to any person obtaining a copy
    of this software and associated documentation files (the "Software"),
    to deal in the Software without restriction, including without limitation
    the rights to use, copy, modify, merge, publish, distribute, sublicense,
    and/or sell copies of the Software, and to permit persons to whom
    the Software is furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included
    in all copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
    THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
    FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
    IN THE SOFTWARE.
*/

#include "../src/nn.h"
#include "../src/reqrep.h"
#include "../src/utils/thread.c"

#include "testutil.h"

#define SOCKET_ADDRESS "inproc://test"

#define THREADS 64

static char canary[64];
static char payload[2 * 4096];

static void client(void *cookie)
{
    int req;

    req = test_socket (AF_SP, NN_REQ);
    test_connect (req, SOCKET_ADDRESS);
    nn_sleep (50);

    test_send (req, payload);
    test_recv (req, canary);
    nn_close (req);
}

static void server(void *cookie)
{
    int i;
    int bytes;
    char *buf = NULL;
    int sock = *(int*) cookie;

    for (i = 0; i < THREADS; ++i) {
        test_recv(sock, payload);
        test_send(sock, canary);
    }
}

int main (int argc, char *argv[])
{
    int i;
    int rep;

    struct nn_thread srv_thread;
    struct nn_thread cli_threads[THREADS];

    srand(time(NULL));
    for (i = 0; i < sizeof (canary); ++i)
        canary[i] = rand() | 1;

    for (i = 0; i < sizeof (payload) / sizeof (canary); ++i)
        memcpy(payload + i * sizeof (canary), canary, sizeof (canary));

    rep = test_socket (AF_SP, NN_REP);
    test_bind (rep, SOCKET_ADDRESS);
    nn_sleep (50);

    nn_thread_init(&srv_thread, server, &rep);

    for (i = 0; i != THREADS; ++i)
        nn_thread_init(&cli_threads[i], client, NULL);
    for (i = 0; i != THREADS; ++i)
        nn_thread_term(&cli_threads[i]);

    nn_thread_term(&srv_thread);
    nn_close (rep);
}
