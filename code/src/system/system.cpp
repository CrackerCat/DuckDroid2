// SPDX-License-Identifier: GPL-2.0-only
#include "system/system.hpp"

#include <atomic>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <unistd.h>

#include "common/log/logger.hpp"
#include "common/uds/socket.hpp"
#include "protocol/frame.hpp"

namespace duckemu::system
{

struct System::Impl
{
	struct Connection
	{
		common::uds::Socket socket;
		size_t app_index = 0;
		size_t worker_index = 0;
		std::mutex write_mutex; // guards closed and socket writes
		bool closed = false;
		std::atomic<bool> finished{ false };
		std::thread thread;
		// Guarded by Impl::conn_map_mutex.
		std::unordered_set<uint32_t> conn_ids;
	};

	struct AppRuntime
	{
		WorkerSpec spec;
		std::string socket_path;
		common::uds::Socket listener;
		WorkerPool pool;
	};

	SystemConfig config;
	std::vector<std::unique_ptr<AppRuntime> > apps;
	common::uds::Socket stop_reader;
	common::uds::Socket stop_writer;
	std::atomic<bool> stopping{ false };
	std::atomic<uint32_t> next_conn_id{ 1 };

	std::mutex conn_map_mutex;
	std::unordered_map<uint32_t, std::weak_ptr<Connection> > conn_map;
	std::mutex connections_mutex;
	std::vector<std::shared_ptr<Connection> > connections;

	bool Start(std::string *error);
	void StartConnection(size_t app_index, common::uds::Socket client);
	void ConnectionLoop(std::shared_ptr<Connection> conn);
	void CloseConnection(const std::shared_ptr<Connection> &conn);
	void OnResponse(uint32_t conn_id, std::string payload);
	void OnLoss(size_t app_index, size_t worker_index);
	void PruneConnections();
	void StopAll();
	AppRuntime *FindApp(std::string_view app) const;
};

bool System::Impl::Start(std::string *error)
{
	if (config.apps.empty())
	{
		*error = "system needs at least one app";
		return false;
	}
	if (!config.socket_dir.empty())
	{
		if (::mkdir(config.socket_dir.c_str(), 0700) < 0 && errno != EEXIST)
		{
			*error = std::string("mkdir(") + config.socket_dir + "): " + std::strerror(errno);
			return false;
		}
	}
	int stop_fds[2] = { -1, -1 };
	if (::pipe2(stop_fds, O_CLOEXEC) < 0)
	{
		*error = std::string("pipe2: ") + std::strerror(errno);
		return false;
	}
	stop_reader = common::uds::Socket(stop_fds[0]);
	stop_writer = common::uds::Socket(stop_fds[1]);

	for (size_t index = 0; index < config.apps.size(); ++index)
	{
		AppConfig &app_config = config.apps[index];
		auto app = std::make_unique<AppRuntime>();
		app->spec = app_config.worker;
		if (app->spec.app.empty())
		{
			*error = "app is missing an id";
			return false;
		}
		app->socket_path = app_config.socket_path.empty()
			? config.socket_dir + "/" + app->spec.app + ".sock"
			: app_config.socket_path;

		if (!app->pool.start(app->spec,
				[this](uint32_t conn_id, std::string payload) {
					OnResponse(conn_id, std::move(payload));
				},
				[this, index](size_t worker_index) { OnLoss(index, worker_index); }, error))
		{
			return false;
		}
		if (!app->listener.listen_on(app->socket_path, 64, error))
		{
			app->pool.stop();
			return false;
		}
		DUCKEMU_INFO("system: %s listening on %s (workers=%zu instances=%u)",
			app->spec.app.c_str(), app->socket_path.c_str(), app->pool.size(),
			app->spec.instances);
		apps.push_back(std::move(app));
	}
	return true;
}

void System::Impl::StartConnection(size_t app_index, common::uds::Socket client)
{
	AppRuntime &app = *apps[app_index];
	const int worker_index = app.pool.pick();
	if (worker_index < 0)
	{
		DUCKEMU_WARN("system: no healthy worker for %s; dropping connection",
			app.spec.app.c_str());
		return;
	}
	auto conn = std::make_shared<Connection>();
	conn->app_index = app_index;
	conn->worker_index = static_cast<size_t>(worker_index);
	conn->socket = std::move(client);
	{
		std::lock_guard<std::mutex> lock(connections_mutex);
		connections.push_back(conn);
	}
	conn->thread = std::thread([this, conn]() { ConnectionLoop(conn); });
}

void System::Impl::ConnectionLoop(std::shared_ptr<Connection> conn)
{
	AppRuntime &app = *apps[conn->app_index];
	while (true)
	{
		std::string payload;
		std::string error;
		if (!protocol::read_frame(conn->socket, &payload, &error))
		{
			break;
		}
		const uint32_t conn_id = next_conn_id.fetch_add(1);
		{
			std::lock_guard<std::mutex> lock(conn_map_mutex);
			conn_map[conn_id] = conn;
			conn->conn_ids.insert(conn_id);
		}
		if (!app.pool.route(conn->worker_index, conn_id, payload, &error))
		{
			DUCKEMU_WARN("system: routing to %s failed: %s", app.spec.app.c_str(),
				error.c_str());
			{
				std::lock_guard<std::mutex> lock(conn_map_mutex);
				conn_map.erase(conn_id);
				conn->conn_ids.erase(conn_id);
			}
			break;
		}
	}
	CloseConnection(conn);
	conn->finished.store(true);
}

void System::Impl::CloseConnection(const std::shared_ptr<Connection> &conn)
{
	std::lock_guard<std::mutex> write_lock(conn->write_mutex);
	if (conn->closed)
	{
		return;
	}
	conn->closed = true;
	if (conn->socket.valid())
	{
		::shutdown(conn->socket.fd(), SHUT_RDWR);
	}
	std::lock_guard<std::mutex> map_lock(conn_map_mutex);
	for (const uint32_t conn_id : conn->conn_ids)
	{
		conn_map.erase(conn_id);
	}
	conn->conn_ids.clear();
}

void System::Impl::OnResponse(uint32_t conn_id, std::string payload)
{
	std::shared_ptr<Connection> conn;
	{
		std::lock_guard<std::mutex> lock(conn_map_mutex);
		const auto found = conn_map.find(conn_id);
		if (found != conn_map.end())
		{
			conn = found->second.lock();
			conn_map.erase(found);
		}
	}
	if (!conn)
	{
		return;
	}
	std::lock_guard<std::mutex> write_lock(conn->write_mutex);
	if (conn->closed)
	{
		return;
	}
	std::string error;
	if (!protocol::write_frame(conn->socket, payload, &error))
	{
		conn->closed = true;
	}
	std::lock_guard<std::mutex> map_lock(conn_map_mutex);
	conn->conn_ids.erase(conn_id);
}

void System::Impl::OnLoss(size_t app_index, size_t worker_index)
{
	std::vector<std::shared_ptr<Connection> > affected;
	{
		std::lock_guard<std::mutex> lock(connections_mutex);
		for (const auto &conn : connections)
		{
			if (conn->app_index == app_index && conn->worker_index == worker_index &&
				!conn->finished.load())
			{
				affected.push_back(conn);
			}
		}
	}
	for (const auto &conn : affected)
	{
		CloseConnection(conn);
	}
}

void System::Impl::PruneConnections()
{
	std::vector<std::shared_ptr<Connection> > finished;
	{
		std::lock_guard<std::mutex> lock(connections_mutex);
		auto it = connections.begin();
		while (it != connections.end())
		{
			if ((*it)->finished.load())
			{
				finished.push_back(*it);
				it = connections.erase(it);
			}
			else
			{
				++it;
			}
		}
	}
	for (const auto &conn : finished)
	{
		if (conn->thread.joinable())
		{
			conn->thread.join();
		}
	}
}

void System::Impl::StopAll()
{
	for (const auto &app : apps)
	{
		app->pool.stop();
		app->listener.close();
	}
	std::vector<std::shared_ptr<Connection> > snapshot;
	{
		std::lock_guard<std::mutex> lock(connections_mutex);
		// swap, not copy: shared_ptr vector copy-assignment trips a GCC 16
		// -Wnull-dereference false positive inside libstdc++.
		snapshot.swap(connections);
	}
	for (const auto &conn : snapshot)
	{
		CloseConnection(conn);
	}
	for (const auto &conn : snapshot)
	{
		if (conn->thread.joinable())
		{
			conn->thread.join();
		}
	}
}

System::Impl::AppRuntime *System::Impl::FindApp(std::string_view app) const
{
	for (const auto &runtime : apps)
	{
		if (runtime->spec.app == app)
		{
			return runtime.get();
		}
	}
	return nullptr;
}

System::System() : impl_(std::make_unique<Impl>()) {}

System::~System()
{
	impl_->StopAll();
}

std::unique_ptr<System> System::create(const SystemConfig &config, std::string *error)
{
	std::unique_ptr<System> system(new System());
	system->impl_->config = config;
	if (!system->impl_->Start(error))
	{
		return nullptr;
	}
	return system;
}

bool System::run(std::string *error)
{
	std::vector<pollfd> descriptors;
	descriptors.reserve(impl_->apps.size() + 1);
	for (const auto &app : impl_->apps)
	{
		pollfd descriptor = { app->listener.fd(), POLLIN, 0 };
		descriptors.push_back(descriptor);
	}
	pollfd stop_descriptor = { impl_->stop_reader.fd(), POLLIN, 0 };
	descriptors.push_back(stop_descriptor);

	while (!impl_->stopping.load())
	{
		const int ready = ::poll(descriptors.data(), descriptors.size(), 500);
		if (ready < 0)
		{
			if (errno == EINTR)
			{
				continue;
			}
			*error = std::string("poll: ") + std::strerror(errno);
			break;
		}
		if (descriptors.back().revents != 0)
		{
			break;
		}
		if (ready > 0)
		{
			for (size_t index = 0; index < impl_->apps.size(); ++index)
			{
				if ((descriptors[index].revents & POLLIN) == 0)
				{
					continue;
				}
				std::string accept_error;
				common::uds::Socket client = impl_->apps[index]->listener.accept(&accept_error);
				if (!client.valid())
				{
					DUCKEMU_WARN("system: accept failed: %s", accept_error.c_str());
					continue;
				}
				impl_->StartConnection(index, std::move(client));
			}
		}
		impl_->PruneConnections();
	}
	impl_->StopAll();
	return true;
}

void System::stop()
{
	if (impl_->stopping.exchange(true))
	{
		return;
	}
	const char byte = 1;
	std::string error;
	impl_->stop_writer.write_all(&byte, 1, &error);
}

std::string System::socket_path(std::string_view app) const
{
	const Impl::AppRuntime *runtime = impl_->FindApp(app);
	return runtime == nullptr ? std::string() : runtime->socket_path;
}

std::vector<int> System::worker_pids(std::string_view app) const
{
	const Impl::AppRuntime *runtime = impl_->FindApp(app);
	return runtime == nullptr ? std::vector<int>() : runtime->pool.pids();
}

} // namespace duckemu::system
