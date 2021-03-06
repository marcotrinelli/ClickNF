/*
 * tcpepollserver.{cc,hh} -- a generic TCP server using epoll_wait()
 * Rafael Laufer, Massimo Gallo
 *
 * Copyright (c) 2017 Nokia Bell Labs
 *
 * Redistribution and use in source and binary forms, with or without modification, are permitted provided that the following conditions are met:
 * 
 * 1. Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following disclaimer 
 *    in the documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the copyright holder nor the names of its contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
 * EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 *
 */


#include <click/config.h>
#include <click/args.hh>
#include <click/error.hh>
#include <click/router.hh>
#include <click/routervisitor.hh>
#include <click/hashtable.hh>
#include <clicknet/ip.h>
#include <clicknet/tcp.h>
#include <click/master.hh>
#include <click/standard/scheduleinfo.hh>
#include "tcpsocket.hh"
#include "tcpepollserver.hh"
#include "util.hh"
CLICK_DECLS

TCPEpollServer::TCPEpollServer()
	: _verbose(false), _batch(1)
{
}

int
TCPEpollServer::configure(Vector<String> &conf, ErrorHandler *errh)
{
	if (Args(conf, this, errh)
		.read_mp("ADDRESS", _addr)
		.read_mp("PORT", _port)
		.read("VERBOSE", _verbose)
		.read("BATCH", _batch)
		.read("PID", _pid)
		.complete() < 0)
		return -1;
	
	return 0;
}

int
TCPEpollServer::initialize(ErrorHandler *errh)
{
	int r = TCPApplication::initialize(errh);
	if (r < 0)
		return r;

	// Get the number of threads
	_nthreads = master()->nthreads();

	// Allocate thread data
	_thread = new ThreadData[_nthreads];
	click_assert(_thread);
	
	// Start per-core tasks
	for (uint32_t c = 0; c < _nthreads; c++) {
		BlockingTask *t = new BlockingTask(this);
		_thread[c].task = t;
		ScheduleInfo::initialize_task(this, t, errh);
		t->move_thread(c);	
	}
	
	return 0;
}


bool
TCPEpollServer::run_task(Task *)
{
	unsigned c = click_current_cpu_id();
	ThreadData *t = &_thread[c];
	
	// Socket
	t->lfd = click_socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
	if (t->lfd < 0) {
		perror("socket");
		return false;
	}
	if (_verbose)
		click_chatter("%s: listen sockfd %d", class_name(), t->lfd);

	// Bind
	if (click_bind(t->lfd, _addr, _port) < 0) {
		perror("bind");
		return false;
	}
	if (_verbose)
		click_chatter("%s: bounded to %s, port %d", \
		                          class_name(), _addr.unparse().c_str(), _port);

	// Listen
	if (click_listen(t->lfd, 8192) < 0) {
		perror("listen");
		return false;
	}
	if (_verbose)
		click_chatter("%s: listening at %s, port %u", \
		                          class_name(), _addr.unparse().c_str(), _port);

	// Create epollfd 
	t->epfd = click_epoll_create(1);
	if (t->epfd < 0) {
		perror("epoll_create");
		return false;
	}
	
	if (_verbose)
		click_chatter("%s: created epoll fd %d", class_name(), t->epfd);

	// Create epoll event
	struct epoll_event ev;
	ev.events = EPOLLIN;
	ev.data.fd = t->lfd;
        
	// Add listener file descriptor to the epollfd list
	if (click_epoll_ctl(t->epfd, EPOLL_CTL_ADD, t->lfd, &ev) < 0) {
		perror("epoll_ctl");
		return false;
	}

	int MAXevents = 4096;
	struct epoll_event events[MAXevents];

	for (;;) {
		// Poll active file descriptors
		int n = click_epoll_wait(t->epfd, events, MAXevents, -1);
		if (n < 0) {
			perror("epoll");
			return false;
		}
		if (_verbose) 
			click_chatter("%s: epoll %d events", class_name(), n);

		// Go over each socket file descriptor
		for (int i = 0; i < n; i++)
			selected(events[i].data.fd, events[i].events);

		// Check if we should stop
		if (home_thread()->stop_flag())
			break;
	}

	// Remove listener from the list of watched file descriptors
	click_epoll_ctl(t->epfd, EPOLL_CTL_DEL, t->lfd, NULL);
	click_epoll_close(t->epfd);

	if (_verbose)
		click_chatter("%s: closing sockfd %d", class_name(), t->lfd);
		
	click_close(t->lfd);

	return false;
}

void
TCPEpollServer::selected(int sockfd, int revents)
{
	unsigned c = click_current_cpu_id();
	ThreadData *t = &_thread[c];
	
	if (revents & EPOLLIN) {
		// Handle new and already established connections
		if (sockfd == t->lfd) {
			// Accept the connection
			IPAddress addr;
			uint16_t port = 0;
			
			int newfd = click_accept(t->lfd, addr, port);
			if (newfd == -1) {
				perror("accept");
				return;
			}

			struct epoll_event ev;
			ev.events = EPOLLIN;
			ev.data.fd = newfd;

			if (click_epoll_ctl(t->epfd, EPOLL_CTL_ADD, newfd, &ev) < 0) {
				perror("epoll_ctl");
				click_close(newfd);
				return;
			}

			if (_verbose)
				click_chatter("%s: accepted fd %d from %s port %u",
				             class_name(), newfd, addr.unparse().c_str(), port);

			Packet *p = Packet::make((const void *)NULL, 0);
			SET_TCP_SOCKFD_ANNO(p, newfd);
			SET_TCP_SOCK_ADD_FLAG_ANNO(p);
			output(TCP_EPOLL_SERVER_OUT_APP_PORT).push(p);
		}
		else {
			if (_verbose)
				click_chatter("%s: event on sockfd = %d", class_name(), sockfd);

			Packet *p = click_pull(sockfd, _batch);
			if (!p) {
				perror("pull");

				if (click_epoll_ctl(t->epfd, EPOLL_CTL_DEL, sockfd, NULL) < 0)
					perror("epoll_ctl");

				click_close(sockfd);
				p = Packet::make((const void *)NULL, 0);
				SET_TCP_SOCK_DEL_FLAG_ANNO(p);
			}
			else if (!p->length()) {
				if (click_epoll_ctl(t->epfd, EPOLL_CTL_DEL, sockfd, NULL) < 0)
					perror("epoll_ctl");

				click_close(sockfd);
				SET_TCP_SOCK_DEL_FLAG_ANNO(p);
			}
			
			SET_TCP_SOCKFD_ANNO(p, sockfd);
			output(TCP_EPOLL_SERVER_OUT_APP_PORT).push(p);
		}
	}

	// Check for errors
	if (revents & (EPOLLERR|EPOLLHUP)) {
		if (_verbose)
			click_chatter("%s: closing fd %d due to error",class_name(),sockfd);

		// Remove sockfd from epoll
		if (click_epoll_ctl(t->epfd, EPOLL_CTL_DEL, sockfd, NULL) < 0)
			perror("epoll_ctl");

		// Close connection
		click_close(sockfd); //This could be left to the app

		// Notify application
		Packet *p = Packet::make((const void *)NULL, 0);
		SET_TCP_SOCKFD_ANNO(p, sockfd);
		SET_TCP_SOCK_DEL_FLAG_ANNO(p);
		output(TCP_EPOLL_SERVER_OUT_APP_PORT).push(p);
	}
}

void
TCPEpollServer::push(int port, Packet *p)
{
	unsigned c = click_current_cpu_id();
	ThreadData *t = &_thread[c];
	
	//TODO treat packets in batch
	int sockfd = TCP_SOCKFD_ANNO(p);

	if (port != TCP_EPOLL_SERVER_IN_APP_PORT) {
		p->kill();
		return;
	}

	if (TCP_SOCK_ADD_FLAG_ANNO(p)) {
		struct epoll_event ev;
		ev.events = EPOLLIN;
		ev.data.fd = sockfd;

		if (click_epoll_ctl(t->epfd, EPOLL_CTL_ADD, sockfd, &ev) < 0) {
			perror("epoll_ctl");
			p->kill();
			return;
		}

		p->kill();
		return;
	}
	if (TCP_SOCK_DEL_FLAG_ANNO(p)) {
		if (click_epoll_ctl(t->epfd, EPOLL_CTL_DEL, sockfd, NULL) < 0)
			perror("epoll_ctl");

		click_close(sockfd);
		p->kill();
		return;
	}

	if (!p->length()){
		p->kill();
		return;
	}

	click_push(sockfd, p);

	if (errno) {
		perror("push");
		p->kill();
	}
}

CLICK_ENDDECLS
EXPORT_ELEMENT(TCPEpollServer)
ELEMENT_REQUIRES(TCPApplication)
