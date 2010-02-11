/*       +------------------------------------+
 *       | Inspire Internet Relay Chat Daemon |
 *       +------------------------------------+
 *
 *  InspIRCd: (C) 2002-2010 InspIRCd Development Team
 * See: http://wiki.inspircd.org/Credits
 *
 * This program is free but copyrighted software; see
 *            the file COPYING for details.
 *
 * ---------------------------------------------------
 */

#include "inspircd.h"
#include <zlib.h>
#include <iostream>

/* $ModDesc: Provides zlib link support for servers */
/* $LinkerFlags: -lz */

/*
 * ZLIB_BEST_COMPRESSION (9) is used for all sending of data with
 * a flush after each chunk. A frame may contain multiple lines
 * and should be treated as raw binary data.
 */

/* Status of a connection */
enum izip_status { IZIP_CLOSED = 0, IZIP_OPEN };

/** Represents an zipped connections extra data
 */
class izip_session
{
 public:
	z_stream c_stream;	/* compression stream */
	z_stream d_stream;	/* uncompress stream */
	izip_status status;	/* Connection status */
	std::string outbuf;	/* Holds output buffer (compressed) */
	std::string inbuf;	/* Holds input buffer (compressed) */
};

class ModuleZLib : public Module
{
	izip_session* sessions;

	/* Used for stats z extensions */
	float total_out_compressed;
	float total_in_compressed;
	float total_out_uncompressed;
	float total_in_uncompressed;

	/* Used for reading data from the wire and compressing data to. */
	char *net_buffer;
	unsigned int net_buffer_size;
 public:

	ModuleZLib()
	{
		sessions = new izip_session[ServerInstance->SE->GetMaxFds()];
		for (int i = 0; i < ServerInstance->SE->GetMaxFds(); i++)
			sessions[i].status = IZIP_CLOSED;

		total_out_compressed = total_in_compressed = 0;
		total_out_uncompressed = total_in_uncompressed = 0;
		Implementation eventlist[] = { I_OnStats };
		ServerInstance->Modules->Attach(eventlist, this, 1);

		// Allocate a buffer which is used for reading and writing data
		net_buffer_size = ServerInstance->Config->NetBufferSize;
		net_buffer = new char[net_buffer_size];
	}

	~ModuleZLib()
	{
		delete[] sessions;
		delete[] net_buffer;
	}

	Version GetVersion()
	{
		return Version("Provides zlib link support for servers", VF_VENDOR);
	}

	/* Handle stats z (misc stats) */
	ModResult OnStats(char symbol, User* user, string_list &results)
	{
		if (symbol == 'z')
		{
			std::string sn = ServerInstance->Config->ServerName;

			/* Yeah yeah, i know, floats are ew.
			 * We used them here because we'd be casting to float anyway to do this maths,
			 * and also only floating point numbers can deal with the pretty large numbers
			 * involved in the total throughput of a server over a large period of time.
			 * (we dont count 64 bit ints because not all systems have 64 bit ints, and floats
			 * can still hold more.
			 */
			float outbound_r = (total_out_compressed / (total_out_uncompressed + 0.001)) * 100;
			float inbound_r = (total_in_compressed / (total_in_uncompressed + 0.001)) * 100;

			float total_compressed = total_in_compressed + total_out_compressed;
			float total_uncompressed = total_in_uncompressed + total_out_uncompressed;

			float total_r = (total_compressed / (total_uncompressed + 0.001)) * 100;

			char outbound_ratio[MAXBUF], inbound_ratio[MAXBUF], combined_ratio[MAXBUF];

			sprintf(outbound_ratio, "%3.2f%%", outbound_r);
			sprintf(inbound_ratio, "%3.2f%%", inbound_r);
			sprintf(combined_ratio, "%3.2f%%", total_r);

			results.push_back(sn+" 304 "+user->nick+" :ZIPSTATS outbound_compressed   = "+ConvToStr(total_out_compressed));
			results.push_back(sn+" 304 "+user->nick+" :ZIPSTATS inbound_compressed    = "+ConvToStr(total_in_compressed));
			results.push_back(sn+" 304 "+user->nick+" :ZIPSTATS outbound_uncompressed = "+ConvToStr(total_out_uncompressed));
			results.push_back(sn+" 304 "+user->nick+" :ZIPSTATS inbound_uncompressed  = "+ConvToStr(total_in_uncompressed));
			results.push_back(sn+" 304 "+user->nick+" :ZIPSTATS percentage_of_original_outbound_traffic        = "+outbound_ratio);
			results.push_back(sn+" 304 "+user->nick+" :ZIPSTATS percentage_of_orignal_inbound_traffic         = "+inbound_ratio);
			results.push_back(sn+" 304 "+user->nick+" :ZIPSTATS total_size_of_original_traffic        = "+combined_ratio);
			return MOD_RES_PASSTHRU;
		}

		return MOD_RES_PASSTHRU;
	}

	void OnStreamSocketConnect(StreamSocket* user)
	{
		OnStreamSocketAccept(user, 0, 0);
	}

	void OnRawSocketAccept(StreamSocket* user, irc::sockets::sockaddrs*, irc::sockets::sockaddrs*)
	{
		int fd = user->GetFd();

		izip_session* session = &sessions[fd];

		/* Just in case... */
		session->outbuf.clear();

		session->c_stream.zalloc = (alloc_func)0;
		session->c_stream.zfree = (free_func)0;
		session->c_stream.opaque = (voidpf)0;

		session->d_stream.zalloc = (alloc_func)0;
		session->d_stream.zfree = (free_func)0;
		session->d_stream.opaque = (voidpf)0;

		/* If we cant call this, well, we're boned. */
		if (inflateInit(&session->d_stream) != Z_OK)
		{
			session->status = IZIP_CLOSED;
			return;
		}

		/* Same here */
		if (deflateInit(&session->c_stream, Z_BEST_COMPRESSION) != Z_OK)
		{
			inflateEnd(&session->d_stream);
			session->status = IZIP_CLOSED;
			return;
		}

		/* Just in case, do this last */
		session->status = IZIP_OPEN;
	}

	void OnStreamSocketClose(StreamSocket* user)
	{
		int fd = user->GetFd();
		CloseSession(&sessions[fd]);
	}

	int OnStreamSocketRead(StreamSocket* user, std::string& recvq)
	{
		int fd = user->GetFd();
		/* Find the sockets session */
		izip_session* session = &sessions[fd];

		if (session->status == IZIP_CLOSED)
			return -1;

		if (session->inbuf.empty())
		{
			/* Read read_buffer_size bytes at a time to the buffer (usually 2.5k) */
			int readresult = read(fd, net_buffer, net_buffer_size);

			if (readresult < 0)
			{
				if (errno == EINTR || errno == EAGAIN)
					return 0;
			}
			if (readresult <= 0)
				return -1;

			total_in_compressed += readresult;

			/* Copy the compressed data into our input buffer */
			session->inbuf.append(net_buffer, readresult);
		}

		size_t in_len = session->inbuf.length();
		char* buffer = ServerInstance->GetReadBuffer();
		int count = ServerInstance->Config->NetBufferSize;

		/* Prepare decompression */
		session->d_stream.next_in = (Bytef *)session->inbuf.c_str();
		session->d_stream.avail_in = in_len;

		session->d_stream.next_out = (Bytef*)buffer;
		/* Last byte is reserved for NULL terminating that beast */
		session->d_stream.avail_out = count - 1;

		/* Z_SYNC_FLUSH: Do as much as possible */
		int ret = inflate(&session->d_stream, Z_SYNC_FLUSH);
		/* TODO CloseStream() in here at random places */
		switch (ret)
		{
			case Z_NEED_DICT:
			case Z_STREAM_ERROR:
				/* This is one of the 'not supposed to happen' things.
				 * Memory corruption, anyone?
				 */
				Error(session, "General Error. This is not supposed to happen :/");
				break;
			case Z_DATA_ERROR:
				Error(session, "Decompression failed, malformed data");
				break;
			case Z_MEM_ERROR:
				Error(session, "Out of memory");
				break;
			case Z_BUF_ERROR:
				/* This one is non-fatal, buffer is just full
				 * (can't happen here).
				 */
				Error(session, "Internal error. This is not supposed to happen.");
				break;
			case Z_STREAM_END:
				/* This module *never* generates these :/ */
				Error(session, "End-of-stream marker received");
				break;
			case Z_OK:
				break;
			default:
				/* NO WAI! This can't happen. All errors are handled above. */
				Error(session, "Unknown error");
				break;
		}
		if (ret != Z_OK)
		{
			return -1;
		}

		/* Update the inbut buffer */
		unsigned int input_compressed = in_len - session->d_stream.avail_in;
		session->inbuf = session->inbuf.substr(input_compressed);

		/* Update counters (Old size - new size) */
		unsigned int uncompressed_length = (count - 1) - session->d_stream.avail_out;
		total_in_uncompressed += uncompressed_length;

		/* Null-terminate the buffer -- this doesnt harm binary data */
		recvq.append(buffer, uncompressed_length);
		return 1;
	}

	int OnStreamSocketWrite(StreamSocket* user, std::string& sendq)
	{
		int fd = user->GetFd();
		izip_session* session = &sessions[fd];

		if(session->status != IZIP_OPEN)
			/* Seriously, wtf? */
			return -1;

		int ret;

		/* This loop is really only supposed to run once, but in case 'compr'
		 * is filled up somehow we are prepared to handle this situation.
		 */
		unsigned int offset = 0;
		do
		{
			/* Prepare compression */
			session->c_stream.next_in = (Bytef*)sendq.data() + offset;
			session->c_stream.avail_in = sendq.length() - offset;

			session->c_stream.next_out = (Bytef*)net_buffer;
			session->c_stream.avail_out = net_buffer_size;

			/* Compress the text */
			ret = deflate(&session->c_stream, Z_SYNC_FLUSH);
			/* TODO CloseStream() in here at random places */
			switch (ret)
			{
				case Z_OK:
					break;
				case Z_BUF_ERROR:
					/* This one is non-fatal, buffer is just full
					 * (can't happen here).
					 */
					Error(session, "Internal error. This is not supposed to happen.");
					break;
				case Z_STREAM_ERROR:
					/* This is one of the 'not supposed to happen' things.
					 * Memory corruption, anyone?
					 */
					Error(session, "General Error. This is also not supposed to happen.");
					break;
				default:
					Error(session, "Unknown error");
					break;
			}

			if (ret != Z_OK)
				return 0;

			/* Space before - space after stuff was added to this */
			unsigned int compressed = net_buffer_size - session->c_stream.avail_out;
			unsigned int uncompressed = sendq.length() - session->c_stream.avail_in;

			/* Make it skip the data which was compressed already */
			offset += uncompressed;

			/* Update stats */
			total_out_uncompressed += uncompressed;
			total_out_compressed += compressed;

			/* Add compressed to the output buffer */
			session->outbuf.append((const char*)net_buffer, compressed);
		} while (session->c_stream.avail_in != 0);

		/* Lets see how much we can send out */
		ret = write(fd, session->outbuf.data(), session->outbuf.length());

		/* Check for errors, and advance the buffer if any was sent */
		if (ret > 0)
			session->outbuf = session->outbuf.substr(ret);
		else if (ret < 1)
		{
			if (errno == EAGAIN)
				return 0;
			else
			{
				session->outbuf.clear();
				return -1;
			}
		}

		return 1;
	}

	void Error(izip_session* session, const std::string &text)
	{
		ServerInstance->SNO->WriteToSnoMask('l', "ziplink error: " + text);
	}

	void CloseSession(izip_session* session)
	{
		if (session->status == IZIP_OPEN)
		{
			session->status = IZIP_CLOSED;
			session->outbuf.clear();
			inflateEnd(&session->d_stream);
			deflateEnd(&session->c_stream);
		}
	}

};

MODULE_INIT(ModuleZLib)
