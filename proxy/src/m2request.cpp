/*
 * Copyright (C) 2012-2013 Fan Out Networks, Inc.
 *
 * This file is part of Pushpin.
 *
 * Pushpin is free software: you can redistribute it and/or modify it under
 * the terms of the GNU Affero General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * Pushpin is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU Affero General Public License for
 * more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "m2request.h"

#include <assert.h>
#include <QPointer>
#include <QFile>
#include <QFileSystemWatcher>
#include "packet/m2requestpacket.h"
#include "log.h"
#include "m2manager.h"

#define BUFFER_SIZE 200000

class M2Request::Private : public QObject
{
	Q_OBJECT

public:
	M2Request *q;
	M2Manager *manager;
	M2Manager *managerForResponse; // even if we unlink, we'll keep a ref here
	bool active;
	M2RequestPacket p;
	bool isHttps;
	bool finished;
	QFile *file;
	QFileSystemWatcher *watcher;
	bool fileComplete;
	QByteArray in;
	int rcount;

	Private(M2Request *_q) :
		QObject(_q),
		q(_q),
		manager(0),
		managerForResponse(0),
		active(false),
		isHttps(false),
		finished(false),
		file(0),
		watcher(0),
		fileComplete(false),
		rcount(0)
	{
	}

	~Private()
	{
		cleanup();
	}

	void cleanup()
	{
		if(watcher)
		{
			delete watcher;
			watcher = 0;
		}

		if(file)
		{
			delete file;
			file = 0;
		}

		if(manager)
		{
			manager->unlink(q);
			manager = 0;
		}
	}

	QByteArray read(int size)
	{
		if(size != -1)
			size = qMin(size, in.size());
		else
			size = in.size();

		if(size == 0)
			return QByteArray();

		QByteArray out = in.mid(0, size);
		in = in.mid(size);

		if(file)
			QMetaObject::invokeMethod(this, "doRead", Qt::QueuedConnection);

		return out;
	}

	bool tryReadFile()
	{
		int avail = BUFFER_SIZE - in.size();
		if(avail <= 0)
			return true;

		assert(file);

		QByteArray buf(avail, 0);
		qint64 pos = file->pos();
		qint64 ret = file->read(buf.data(), avail);
		if(ret < 0)
			return false;

		buf.resize(ret);

		if(file->atEnd())
		{
			// take a step back from EOF
			if(!file->seek(pos + ret))
				return false;
		}

		if(!buf.isEmpty())
		{
			in += buf;
			rcount += buf.size();

			if(fileComplete && rcount >= file->size())
			{
				delete watcher;
				watcher = 0;
				delete file;
				file = 0;
			}

			if(active)
			{
				QPointer<QObject> self = this;
				emit q->readyRead();
				if(!self)
					return true;

				tryFinish();
			}
		}

		return true;
	}

	void tryFinish()
	{
		// file is null once everything is in a local buffer, or if a file wasn't used at all
		if(active && !file)
		{
			cleanup();
			finished = true;
			emit q->finished();
		}
	}

	void disconnected()
	{
		cleanup();
		emit q->error();
	}

public slots:
	void watcher_fileChanged(const QString &path)
	{
		Q_UNUSED(path);

		if(in.size() < BUFFER_SIZE)
			doRead();
	}

	void doActivate()
	{
		QPointer<QObject> self = this;

		if(!in.isEmpty())
		{
			emit q->readyRead();
			if(!self)
				return;
		}

		tryFinish();
	}

	void doRead()
	{
		if(!tryReadFile())
		{
			cleanup();
			log_error("unable to read file: %s", qPrintable(p.uploadFile));
			emit q->error();
		}
	}
};

M2Request::M2Request(QObject *parent) :
	QObject(parent)
{
	d = new Private(this);
}

M2Request::~M2Request()
{
	delete d;
}

M2Request::Rid M2Request::rid() const
{
	return Rid(d->p.sender, d->p.id);
}

bool M2Request::isHttps() const
{
	return d->isHttps;
}

bool M2Request::isFinished() const
{
	return d->finished;
}

QString M2Request::method() const
{
	return d->p.method;
}

QByteArray M2Request::path() const
{
	return d->p.path;
}

const HttpHeaders & M2Request::headers() const
{
	return d->p.headers;
}

QByteArray M2Request::read(int size)
{
	return d->read(size);
}

int M2Request::actualContentLength() const
{
	return d->rcount;
}

M2Response *M2Request::createResponse()
{
	assert(d->managerForResponse);
	return d->managerForResponse->createResponse(Rid(d->p.sender, d->p.id));
}

M2Manager *M2Request::managerForResponse()
{
	return d->managerForResponse;
}

bool M2Request::handle(M2Manager *manager, const M2RequestPacket &packet, bool https)
{
	d->manager = manager;
	d->managerForResponse = manager;
	d->p = packet;
	d->isHttps = https;

	if(!d->p.uploadFile.isEmpty())
	{
		// if uploadFile was specified then start monitoring
		d->file = new QFile(d->p.uploadFile, d);
		if(!d->file->open(QFile::ReadOnly))
		{
			d->manager = 0; // no need to unlink, returning false will ensure this
			d->cleanup();
			log_error("unable to open file: %s", qPrintable(d->p.uploadFile));
			return false;
		}

		d->watcher = new QFileSystemWatcher(d);
		connect(d->watcher, SIGNAL(fileChanged(const QString &)), d, SLOT(watcher_fileChanged(const QString &)));
		d->watcher->addPath(d->p.uploadFile);

		if(!d->tryReadFile())
		{
			d->manager = 0; // no need to unlink, returning false will ensure this
			d->cleanup();
			log_error("unable to read file: %s", qPrintable(d->p.uploadFile));
			return false;
		}
	}
	else
	{
		// if no uploadFile was specified then we have the body in the packet and we're done
		d->in = d->p.body;
		d->rcount = d->p.body.size();
		d->p.body.clear();
		d->finished = true;
	}

	return true;
}

void M2Request::activate()
{
	d->active = true;
	QMetaObject::invokeMethod(d, "doActivate", Qt::QueuedConnection);
}

void M2Request::uploadDone()
{
	d->fileComplete = true;

	if(d->rcount >= d->file->size())
	{
		delete d->watcher;
		d->watcher = 0;
		delete d->file;
		d->file = 0;

		d->tryFinish();
	}
}

void M2Request::disconnected()
{
	d->disconnected();
}

#include "m2request.moc"
