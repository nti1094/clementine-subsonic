/* This file is part of Clementine.

   Clementine is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   Clementine is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with Clementine.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef MTPLOADER_H
#define MTPLOADER_H

#include <QObject>

#include <boost/shared_ptr.hpp>

class ConnectedDevice;
class LibraryBackend;
class TaskManager;

class MtpLoader : public QObject {
  Q_OBJECT

public:
  MtpLoader(const QString& hostname, TaskManager* task_manager,
            LibraryBackend* backend, boost::shared_ptr<ConnectedDevice> device);
  ~MtpLoader();

public slots:
  void LoadDatabase();

signals:
  void Error(const QString& message);
  void TaskStarted(int task_id);
  void LoadFinished();

private:
  bool TryLoad();

private:
  boost::shared_ptr<ConnectedDevice> device_;
  QThread* original_thread_;

  QString hostname_;
  TaskManager* task_manager_;
  LibraryBackend* backend_;
};

#endif // MTPLOADER_H