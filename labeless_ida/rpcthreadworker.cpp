/* Labeless
 * by Aliaksandr Trafimchuk
 *
 * Source code released under
 * Creative Commons BY-NC 4.0
 * http://creativecommons.org/licenses/by-nc/4.0
 */

#include "rpcthreadworker.h"

#include "labeless_ida.h"
#include "util/util_ida.h"
#include "util/util_net.h"
#include "util/util_protobuf.h"
#include "../common/cpp/rpc.pb.h"

#include <QApplication>

RpcThreadWorker::RpcThreadWorker(QObject* parent)
	: QObject(parent)
{
	msg("%s\n", Q_FUNC_INFO);
}


RpcThreadWorker::~RpcThreadWorker()
{
	msg("%s\n", Q_FUNC_INFO);
}

void RpcThreadWorker::main()
{
	static const unsigned long waitTime = 1000;

	Labeless& ll = Labeless::instance();
	while (ll.m_Enabled == 1)
	{
		QMutexLocker lock(&ll.m_QueueLock);
		while (ll.m_Queue.empty() && ll.m_Enabled == 1)
		{
			ll.m_QueueCond.wait(&ll.m_QueueLock, waitTime);
		}
		if (ll.m_Enabled != 1)
			break;

		RpcDataPtr pRD;
		unsigned queueSize = 0;
		for (auto it = ll.m_Queue.begin(), end = ll.m_Queue.end(); it != end; ++it)
		{
			RpcDataPtr ptr = *it;
			if (ptr && (!ptr->readyToSendHandler || ptr->readyToSendHandler(ptr)))
			{
				pRD = *it;
				ll.m_Queue.erase(it);
				queueSize = ll.m_Queue.size();
				break;
			}
		}
		lock.unlock();
		if (!pRD)
			continue;

		ll.m_ConfigLock.lock();
		const std::string host = ll.m_Settings.host;
		const uint16_t port = ll.m_Settings.port;
		ll.m_ConfigLock.unlock();

		/* TODO: don't send ExecPyScript command if Olly script is empty
		auto eps = std::dynamic_pointer_cast<ExecPyScript>(pRD->iCmd);
		if (eps && QString::fromStdString(eps->d.ollyScript).trimmed().isEmpty())
		{
			pRD->emitReceived();
			continue;
		}*/

		QString errorMsg;
		SOCKET s = ll.connectToHost(host, port, errorMsg);
		if (INVALID_SOCKET == s)
		{
			if (errorMsg.isEmpty())
				errorMsg = "connectToHost() failed";
			util::ida::addLogMsg("%s\n", errorMsg.toStdString().c_str());
			pRD->emitFailed(errorMsg);
			continue;
		}
		std::shared_ptr<void> guard(nullptr, [s](void*){
#ifdef __NT__
			closesocket(s);
#else
			close(s);
#endif
		}); // clean-up guard
		Q_UNUSED(guard);

		try
		{
			rpc::Execute command;
			command.set_script(pRD->script);
			if (!pRD->scriptExternObj.empty())
				command.set_script_extern_obj(pRD->scriptExternObj);
			command.set_rpc_request(pRD->params);
			if (std::dynamic_pointer_cast<AnalyzeExternalRefs>(pRD->iCmd) && !pRD->jobId && pRD->retryCount == 0)
				command.set_background(true);
			if (pRD->jobId)
				command.set_job_id(pRD->jobId);

			const std::string message = command.SerializeAsString();
			const uint64_t messageLen = static_cast<uint64_t>(message.length());

			if (!util::net::sockSendBuff(s, reinterpret_cast<const char*>(&messageLen), sizeof(messageLen)))
				continue;

			if (!util::net::sockSendString(s, message))
				continue;

			std::string strResponse;
			if (!util::net::sockRecvAll(s, strResponse) || strResponse.empty())
			{
				util::ida::addLogMsg("sockRecvAll() failed, error: %s\n", util::net::wsaErrorToString().toStdString().c_str());
				continue;
			}

			auto response = std::make_shared<rpc::Response>();
			const bool parsedOk = util::protobuf::parseBigMessage(*response, strResponse);
			if (!parsedOk)
			{
				util::ida::addLogMsg("%s: rpc::Response::ParseFromString() failed\n", __FUNCTION__);

#ifdef LABELESS_ADDITIONAL_LOGGING
				do {
					if (auto f = qfopen("c:\\labeless_ida.log", "ab+"))
					{
						qfseek(f, 0, SEEK_END);
						static const std::string s = "\nReceived:\n";
						qfwrite(f, s.c_str(), s.length());
						qfwrite(f, strResponse.c_str(), strResponse.length());
						qfwrite(f, "\r\n", 2);
						qfclose(f);
					}
				} while (0);
#endif // LABELESS_ADDITIONAL_LOGGING
			}
			else
			{
				util::ida::addLogMsg("OK, tasks left: %u\n", queueSize);
			}

			if (parsedOk)
				pRD->response = response;
			pRD->emitReceived();
		}
		catch (...)
		{
			continue;
		}
	}
	moveToThread(qApp->thread());
	deleteLater();
}
