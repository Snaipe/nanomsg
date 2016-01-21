/*
    Copyright (c) 2016 Franklin "Snaipe" Mathieu <franklinmathieu@gmail.com>

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

#ifndef NN_CLEANUP_INCLUDED
#define NN_CLEANUP_INCLUDED

enum nn_cleanup_opt {
    /* Terminate the resources and check the state */
    NN_CLEAN_DEFAULT = 0,

    /* Manually empty and release the contents of any internal collection */
    NN_CLEAN_EMPTY = 1,

    /* Don't check for a valid state */
    NN_CLEAN_NO_CHECK = 2,

    /* Don't cleanup threads or tasks */
    NN_CLEAN_NO_THREADS = 4,


    /* Special masks */
    NN_CLEAN_RESET_UNSAFE = NN_CLEAN_EMPTY
                          | NN_CLEAN_NO_CHECK
                          | NN_CLEAN_NO_THREADS,
};

#endif
