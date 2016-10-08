#include <unistd.h>
#include <stdio.h>
#include <fcntl.h>
#include <string.h>
#include <assert.h>
#include <mutex>
#include <list>
#include <string>
#include <algorithm>
#include "sudo_private.h"
#include "sudo.h"


bool SudoClientConfirm();

namespace Sudo 
{
	static std::mutex s_client_mutex;
	static int s_client_pipe_send = -1;
	static int s_client_pipe_recv = -1;
	static std::string g_curdir_override;
	static struct ListOfStrings : std::list<std::string> {} g_known_curdirs;
	
	static int (*g_sudo_launcher)(int pipe_request, int pipe_reply) = nullptr;
	
	enum ModifyState
	{
		MODIFY_UNDEFINED = 0,
		MODIFY_ALLOWED,
		MODIFY_DENIED		
	};
	struct ThreadRegionCounter
	{
		int count;
		ModifyState modify;
		bool cancelled;
	};
	
	thread_local ThreadRegionCounter thread_client_region_counter = { 0, MODIFY_UNDEFINED, false };
	static int global_client_region_counter = 0;
	static SudoClientMode client_mode = SCM_DISABLE;
	static time_t client_askpass_timeout = 0;
	static time_t client_askpass_timestamp = 0;

	
	enum {
		KNOWN_CURDIRS_LIMIT = 4
	};

	static void CloseClientConnection()
	{
		CheckedCloseFD(s_client_pipe_send);
		CheckedCloseFD(s_client_pipe_recv);
	}
	
	static bool ClientInitSequence()
	{
		try {
			SudoCommand cmd = SUDO_CMD_PING;
			BaseTransaction bt(s_client_pipe_send, s_client_pipe_recv);
			bt.SendPOD(cmd);
			bt.RecvPOD(cmd);

			if (bt.IsFailed() || cmd!=SUDO_CMD_PING)
				throw "ping failed";
				
			if (!g_curdir_override.empty()) {
				cmd = SUDO_CMD_CHDIR;
				bt.SendPOD(cmd);
				bt.SendStr(g_curdir_override.c_str());
				int r = bt.RecvInt();
				if (r == -1) {
					bt.RecvErrno();
				} else {
					std::string str;
					bt.RecvStr(str);
				}
				
				bt.RecvPOD(cmd);
			
				if (bt.IsFailed() || cmd!=SUDO_CMD_CHDIR)
					throw "chdir failed";
					
				fprintf(stderr, "Sudo::ClientInitSequence: chdir='%s' -> %d\n", g_curdir_override.c_str(), r);
			}

		} catch (const char *what) {
			fprintf(stderr, "Sudo::ClientInitSequence - %s\n", what);
			CloseClientConnection();
			return false;
		}
		
		return true;
	}
/*
	static bool ClientConfirm()
	{
		
		
		//actually its possible to show dialog right from here however
		//this would be dangerous due to possible reentrancy problems
		bool out = false;
		try {
			SudoCommand cmd = SUDO_CMD_CONFIRM;
			BaseTransaction bt(s_client_pipe_send, s_client_pipe_recv);
			bt.SendPOD(cmd);
			int r = bt.RecvInt();
			bt.RecvPOD(cmd);
			if (bt.IsFailed() || cmd!=SUDO_CMD_CONFIRM)
				throw "confirm failed";

			out = (r != 0);
		} catch (const char *what) {
			fprintf(stderr, "Sudo::ClientConfirm - %s\n", what);
			CloseClientConnection();
		}
		
		return out;
	}*/

	static bool OpenClientConnection()
	{
		if (!g_sudo_launcher)
			return false;
			
		int req[2] = {-1, -1}, rep[2] = {-1, -1};
		if (pipe(req)==-1)
			return false;
		if (pipe(rep)==-1) {
			CheckedCloseFDPair(req);
			return false;
		}
				
		fcntl(req[1], F_SETFD, FD_CLOEXEC);
		fcntl(rep[0], F_SETFD, FD_CLOEXEC);
				
		if (g_sudo_launcher(req[0], rep[1])==-1) {//likely missing askpass
			perror("g_sudo_launcher\n");
			CheckedCloseFDPair(req);
			CheckedCloseFDPair(rep);
			return false;
		}
		CheckedCloseFD(req[0]);
		CheckedCloseFD(rep[1]);
		s_client_pipe_send = req[1];
		s_client_pipe_recv = rep[0];
		return true;
	}


	static void CheckForCloseClientConnection()
	{
		if (global_client_region_counter != 0)
			return;
			
		if (client_mode != SCM_DISABLE) {
			if (time(nullptr) - client_askpass_timestamp < client_askpass_timeout) {
				return;
			}
		}
			
		CloseClientConnection();
	}
	
	/////////////////////////////////////////////////////////////////////
	
	void ClientCurDirOverrideReset()
	{
		std::lock_guard<std::mutex> lock(s_client_mutex);
		g_curdir_override.clear();
	}
	
	void ClientCurDirOverrideSet(const char *path)
	{
		//fprintf(stderr, "ClientCurDirOverride: %s\n", path);
		std::lock_guard<std::mutex> lock(s_client_mutex);
		g_curdir_override = path;
		if (!g_curdir_override.empty()) {
			g_known_curdirs.push_back(g_curdir_override);
			while (g_known_curdirs.size() > KNOWN_CURDIRS_LIMIT)
				g_known_curdirs.pop_front();
		}
	}
	
	bool ClientCurDirOverrideSetIfKnown(const char *path)
	{
		std::string str = path;
		std::lock_guard<std::mutex> lock(s_client_mutex);
		if (client_mode == SCM_DISABLE)
			return false;

		ListOfStrings::iterator  i = 
			std::find(g_known_curdirs.begin(), g_known_curdirs.end(), str);
		if (i == g_known_curdirs.end())
			return false;
		
		g_curdir_override.swap(str);
		g_known_curdirs.erase(i);
		g_known_curdirs.push_back(g_curdir_override);
		return true;
	}
	
	bool ClientCurDirOverrideQuery(char *path, size_t size)
	{
		std::lock_guard<std::mutex> lock(s_client_mutex);
		if (g_curdir_override.size() >= size ) {
			errno = ERANGE;
			return false;
		}
		
		strcpy(path, g_curdir_override.c_str() );
		return true;
	}
	
	ClientReconstructCurDir::ClientReconstructCurDir(const char * &path)
		: _free_ptr(nullptr), _initial_path(path), _path(path)
	{
		if (path[0] != '/' && path[0]) {
			std::lock_guard<std::mutex> lock(s_client_mutex);
			if (!g_curdir_override.empty()) {
				std::string  str = g_curdir_override;
				if (strcmp(path, ".")==0 || strcmp(path, "./")==0) {
					
				} else if (strcmp(path, "..")==0 || strcmp(path, "../")==0) {
					size_t p = str.rfind('/');
					if (p!=std::string::npos)
						str.resize(p);
				} else if (path[0] == '.' && path[1] == '/') {
					str+= &path[1];
				} else {
					str+= '/';
					str+= &path[0];
				}
				_free_ptr = strdup(str.c_str());
				if (_free_ptr)
					_path = _free_ptr;
			}
		}
	}
	
	ClientReconstructCurDir::~ClientReconstructCurDir()
	{
		_path = _initial_path;
		free(_free_ptr);
	}
	
/////////////////////////
	extern "C" {
		
		
		void sudo_client_configure(SudoClientMode mode, int askpass_timeout)
		{
			std::lock_guard<std::mutex> lock(s_client_mutex);
			client_mode = mode;
			client_askpass_timeout = askpass_timeout;
			CheckForCloseClientConnection();
		}
		
		__attribute__ ((visibility("default"))) void sudo_client(int (*p_sudo_launcher)(int pipe_request, int pipe_reply))
		{
			std::lock_guard<std::mutex> lock(s_client_mutex);
			g_sudo_launcher = p_sudo_launcher;
		}
		
		
		__attribute__ ((visibility("default"))) void sudo_client_region_enter()
		{
			std::lock_guard<std::mutex> lock(s_client_mutex);
			global_client_region_counter++;
			thread_client_region_counter.count++;
		}
		
		__attribute__ ((visibility("default"))) void sudo_client_region_leave()
		{
			std::lock_guard<std::mutex> lock(s_client_mutex);
			assert(global_client_region_counter > 0);
			assert(thread_client_region_counter.count > 0);

			global_client_region_counter--;
			thread_client_region_counter.count--;
			
			if (!thread_client_region_counter.count) {
				thread_client_region_counter.cancelled = false;
				thread_client_region_counter.modify = MODIFY_UNDEFINED;
			}

			CheckForCloseClientConnection();
		}
	}
	

	bool IsSudoRegionActive()
	{
		std::lock_guard<std::mutex> lock(s_client_mutex);
		return (s_client_pipe_send!=-1 && s_client_pipe_recv!=-1 && thread_client_region_counter.count);
	}
	
	bool TouchClientConnection(bool want_modify)
	{
		if (!thread_client_region_counter.count || thread_client_region_counter.cancelled) {
			return false;
		}

		ErrnoSaver es;
		std::lock_guard<std::mutex> lock(s_client_mutex);
		if (client_mode == SCM_DISABLE)
			return false;

		if (s_client_pipe_send==-1 || s_client_pipe_recv==-1) {
			CloseClientConnection();
			if (!OpenClientConnection())
				return false;
				
			if (!ClientInitSequence()) {//likely bad password or Cancel
				thread_client_region_counter.cancelled = true;
				return false; 
			}
			
			//assume user confirmed also modify operation with this password
			if (want_modify && client_mode == SCM_CONFIRM_MODIFY )
				thread_client_region_counter.modify = MODIFY_ALLOWED;

			client_askpass_timestamp = time(nullptr);

		} else if (want_modify && client_mode == SCM_CONFIRM_MODIFY ) {
			if (thread_client_region_counter.modify == MODIFY_UNDEFINED) {
				thread_client_region_counter.modify = 
					SudoClientConfirm() ? MODIFY_ALLOWED : MODIFY_DENIED;
			}
			return (thread_client_region_counter.modify == MODIFY_ALLOWED);
		}

		return true;
	}


	ClientTransaction::ClientTransaction(SudoCommand cmd) : 
		std::lock_guard<std::mutex>(s_client_mutex),
		BaseTransaction(s_client_pipe_send, s_client_pipe_recv), 
		_cmd(cmd)
	{
		try {
			SendPOD(_cmd);
		} catch (const char *) {
			CloseClientConnection();
			throw;
		}
	}
	
	ClientTransaction::~ClientTransaction()
	{
		try {//should catch cuz used from d-tor
			Finalize();
			if (IsFailed())
				CloseClientConnection();
		} catch (const char * what) {
			fprintf(stderr, "ClientTransaction(%u)::Finalize - %s\n", _cmd, what);
			CloseClientConnection();
		}
	}
	
	void ClientTransaction::NewTransaction(SudoCommand cmd)
	{
		Finalize();
		_cmd = cmd;
		SendPOD(_cmd);
	}
	
	void ClientTransaction::Finalize()
	{
		SudoCommand reply;
		RecvPOD(reply);
		if (reply!=_cmd)
			throw "bad reply";
	}
}
