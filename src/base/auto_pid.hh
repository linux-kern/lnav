/**
 * Copyright (c) 2013, Timothy Stack
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * * Redistributions of source code must retain the above copyright notice, this
 * list of conditions and the following disclaimer.
 * * Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 * * Neither the name of Timothy Stack nor the names of its contributors
 * may be used to endorse or promote products derived from this software
 * without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ''AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
 * ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * @file auto_pid.hh
 */

#ifndef auto_pid_hh
#define auto_pid_hh

#include <errno.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "base/result.h"
#include "base/lnav_log.hh"
#include "mapbox/variant.hpp"

enum class process_state {
    RUNNING,
    FINISHED,
};

template<process_state ProcState>
class auto_pid {
public:
    explicit auto_pid(pid_t child, int status = 0) : ap_child(child),
                                                     ap_status(status)
    {};

    auto_pid(const auto_pid &other) = delete;

    auto_pid(auto_pid &&other) noexcept
        : ap_child(std::move(other).release()),
          ap_status(other.ap_status)
    {};

    ~auto_pid() noexcept
    { this->reset(); };

    auto_pid &operator=(auto_pid &&other) noexcept
    {
        this->reset(std::move(other).release());
        this->ap_status = other.ap_status;
        return *this;
    };

    pid_t in() const
    { return this->ap_child; }

    bool in_child() const
    {
        static_assert(ProcState == process_state::RUNNING,
                      "this method is only available in the RUNNING state");
        return this->ap_child == 0;
    };

    pid_t release() &&
    {
        return std::exchange(this->ap_child, -1);
    };

    int status() const
    {
        static_assert(ProcState == process_state::FINISHED,
                      "wait_for_child() must be called first");
        return this->ap_status;
    };

    bool was_normal_exit() const
    {
        static_assert(ProcState == process_state::FINISHED,
                      "wait_for_child() must be called first");
        return WIFEXITED(this->ap_status);
    }

    int exit_status() const
    {
        static_assert(ProcState == process_state::FINISHED,
                      "wait_for_child() must be called first");
        return WEXITSTATUS(this->ap_status);
    }

    using poll_result = mapbox::util::variant<
        auto_pid<process_state::RUNNING>,
        auto_pid<process_state::FINISHED>
    >;

    poll_result poll() && {
        if (this->ap_child != -1) {
            auto rc = waitpid(this->ap_child, &this->ap_status, WNOHANG);

            if (rc <= 0) {
                return std::move(*this);
            }
        }

        return auto_pid<process_state::FINISHED>(
            std::exchange(this->ap_child, -1), this->ap_status);
    }

    auto_pid<process_state::FINISHED> wait_for_child(int options = 0) &&
    {
        if (this->ap_child != -1) {
            while ((waitpid(this->ap_child,
                            &this->ap_status,
                            options)) < 0 && (errno == EINTR)) { ;
            }
        }

        return auto_pid<process_state::FINISHED>(
            std::exchange(this->ap_child, -1), this->ap_status);
    }

    void reset(pid_t child = -1) noexcept
    {
        if (this->ap_child != child) {
            this->ap_status = 0;
            if (ProcState == process_state::RUNNING && this->ap_child != -1) {
                log_debug("sending SIGTERM to child: %d", this->ap_child);
                kill(this->ap_child, SIGTERM);
            }
            this->ap_child = child;
        }
    }

private:
    pid_t ap_child;
    int ap_status{0};
};

namespace lnav {
namespace pid {
Result<auto_pid<process_state::RUNNING>, std::string> from_fork();
}
}

#endif
