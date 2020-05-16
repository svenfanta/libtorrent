/*

Copyright (c) 2019, Paul-Louis Ageneau
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:

    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in
      the documentation and/or other materials provided with the distribution.
    * Neither the name of the author nor the names of its
      contributors may be used to endorse or promote products derived
      from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
POSSIBILITY OF SUCH DAMAGE.

*/

#include "libtorrent/config.hpp" // for TORRENT_USE_RTC

#if TORRENT_USE_RTC

#include "libtorrent/aux_/websocket_stream.hpp"
#include "libtorrent/config.hpp"
#include "libtorrent/debug.hpp"
#include "libtorrent/error.hpp"
#include "libtorrent/io_context.hpp"
#include "libtorrent/parse_url.hpp"
#include "libtorrent/random.hpp"

#include <boost/asio/connect.hpp>
#include <boost/asio/ssl/error.hpp>

#include <algorithm>
#include <tuple>

#include <iostream>

namespace {
	template<class... Ts> struct overloaded : Ts... { using Ts::operator()...; };
	template<class... Ts> overloaded(Ts...) -> overloaded<Ts...>;
}

namespace libtorrent {
namespace aux {

namespace http = boost::beast::http;
namespace error = boost::asio::error;
using namespace std::placeholders;

websocket_stream::websocket_stream(io_context& ios
        , resolver_interface& resolver
        , ssl::context* ssl_ctx
        )
    : m_io_service(ios)
    , m_resolver(resolver)
    , m_ssl_context(ssl_ctx)
    , m_stream(std::in_place_type_t<stream_type>{}, ios)
	, m_open(false)
	, m_connecting(false)
{

}

websocket_stream::~websocket_stream()
{
	close();
}

void websocket_stream::close()
{
	if(m_open || m_connecting) {
		m_open = false;
		m_connecting = false;

		std::visit([&](auto &stream)
		{
			stream.async_close(websocket::close_code::none
				, std::bind(&websocket_stream::on_close, shared_from_this(), _1)
			);
		}
		, m_stream);
	}
}

close_reason_t websocket_stream::get_close_reason()
{
	return close_reason_t::none;
}

void websocket_stream::set_user_agent(std::string user_agent)
{
	m_user_agent = std::move(user_agent);
}

void websocket_stream::do_connect(std::string url)
{
	if(m_open)
	{
		post(m_io_service, std::bind(m_connect_handler, error::already_connected));
		return;
	}
	if(m_connecting)
    {
		post(m_io_service, std::bind(m_connect_handler, error::already_started));
        return;
    }
	m_url = std::move(url);

	std::string protocol, hostname;
	int port;
	error_code ec;
	std::tie(protocol, std::ignore, hostname,  port, m_target) = parse_url_components(m_url, ec);
	if(ec)
	{
		post(m_io_service, std::bind(m_connect_handler, ec));
		return;
	}

	if(protocol == "ws")
	{
		m_stream.emplace<stream_type>(m_io_service);
	}
	else if(protocol == "wss" && m_ssl_context)
	{
		m_stream.emplace<ssl_stream_type>(m_io_service, *m_ssl_context);
	}
	else {
		post(m_io_service, std::bind(m_connect_handler, error::no_protocol_option));
		return;
	}

	if(port <= 0) port = 443;
	if(m_target.empty()) m_target = "/";

	m_connecting = true;
	do_resolve(hostname, port);
}

void websocket_stream::do_resolve(std::string hostname, std::uint16_t port)
{
	m_hostname = std::move(hostname);
	m_port = std::move(port);

	ADD_OUTSTANDING_ASYNC("websocket_stream::on_resolve");
	m_resolver.async_resolve(m_hostname
		, resolver_interface::abort_on_shutdown
        , std::bind(&websocket_stream::on_resolve, shared_from_this(), _1, _2)
    );
}

void websocket_stream::on_resolve(error_code const& ec, std::vector<address> const& addresses)
{
	COMPLETE_ASYNC("websocket_stream::on_resolve");
    if (ec)
    {
    	m_connecting = false;
    	post(m_io_service, std::bind(m_connect_handler, ec));
        return;
    }

    TORRENT_ASSERT(!addresses.empty());

	std::vector<tcp::endpoint> endpoints;
    for (auto const& addr : addresses)
		endpoints.emplace_back(addr, m_port);

	do_tcp_connect(std::move(endpoints));
}

void websocket_stream::do_tcp_connect(std::vector<tcp::endpoint> endpoints)
{
	m_endpoints = std::move(endpoints);

	ADD_OUTSTANDING_ASYNC("websocket_stream::on_tcp_connect");
	auto tcp_stream = std::visit(overloaded {
		[&](stream_type &stream) { return &stream.next_layer(); }
		, [&](ssl_stream_type &stream) { return &stream.next_layer().next_layer(); }
	}
	, m_stream);

	boost::asio::async_connect(*tcp_stream
		, m_endpoints.rbegin()
		, m_endpoints.rend()
		, std::bind(&websocket_stream::on_tcp_connect, shared_from_this(), _1)
	);
}

void websocket_stream::on_tcp_connect(error_code const& ec)
{
	COMPLETE_ASYNC("websocket_stream::on_tcp_connect");
	if (ec)
    {
    	m_connecting = false;
        post(m_io_service, std::bind(m_connect_handler, ec));
        return;
    }

	if (std::holds_alternative<ssl_stream_type>(m_stream)) do_ssl_handshake();
	else do_handshake();
}

void websocket_stream::do_ssl_handshake()
{
	auto& ssl_stream = std::get<ssl_stream_type>(m_stream).next_layer();

	error_code ec;
	ssl::set_host_name(ssl::get_handle(ssl_stream), m_hostname, ec);
    if (ec)
    {
    	m_connecting = false;
        error_code ec{static_cast<int>(ERR_get_error()), error::get_ssl_category()};
        post(m_io_service, std::bind(m_connect_handler, ec));
        return;
    }

	ADD_OUTSTANDING_ASYNC("websocket_stream::on_ssl_handshake");
	ssl_stream.async_handshake(ssl::stream_base::client
			, std::bind(&websocket_stream::on_ssl_handshake, shared_from_this(), _1));
}

void websocket_stream::on_ssl_handshake(error_code const& ec)
{
	COMPLETE_ASYNC("websocket_stream::on_ssl_handshake");
	if (ec)
    {
    	m_connecting = false;
		post(m_io_service, std::bind(m_connect_handler, error::operation_aborted));
        return;
    }

	do_handshake();
}

void websocket_stream::do_handshake()
{
    ADD_OUTSTANDING_ASYNC("websocket_stream::on_handshake");

	auto user_agent_handler = [user_agent = m_user_agent](websocket::request_type& req)
    {
		if(!user_agent.empty()) req.set(http::field::user_agent, user_agent);
    };

	std::visit([&](auto &stream)
	{
#if BOOST_VERSION >= 107000
		stream.set_option(websocket::stream_base::decorator(user_agent_handler));
		stream.async_handshake(m_hostname
			, m_target
			, std::bind(&websocket_stream::on_handshake, shared_from_this(), _1)
		);
#else
		stream.async_handshake_ex(m_hostname
			, m_target
			, user_agent_handler
			, std::bind(&websocket_stream::on_handshake, shared_from_this(), _1)
		);
#endif
	}
	, m_stream);
}

void websocket_stream::on_handshake(error_code const& ec)
{
	COMPLETE_ASYNC("websocket_stream::on_handshake");

	if (ec)
    {
    	m_connecting = false;
		post(m_io_service, std::bind(m_connect_handler, ec));
        return;
    }

	if(!m_connecting)
	{
		post(m_io_service, std::bind(m_connect_handler, error::operation_aborted));
		return;
	}

	m_connecting = false;
	m_open = true;
	post(m_io_service, std::bind(m_connect_handler, error_code{}));
}

void websocket_stream::on_read(error_code const& ec, std::size_t bytes_read, read_handler handler)
{
	if(!m_open) return;

	// Clean close from remote
	if (ec == websocket::error::closed) m_open = false;

	post(m_io_service, std::bind(handler, ec, bytes_read));
}

void websocket_stream::on_write(error_code const& ec, std::size_t bytes_written, write_handler handler)
{
	if(!m_open) return;

	post(m_io_service, std::bind(handler, ec, bytes_written));
}

void websocket_stream::on_close(error_code const&)
{

}

}
}

#endif // TORRENT_USE_RTC

