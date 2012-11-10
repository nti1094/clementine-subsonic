#ifndef SUBSONICSERVICE_H
#define SUBSONICSERVICE_H

#include "internetmodel.h"
#include "internetservice.h"

class SubsonicService : public InternetService
{
  Q_OBJECT

public:
  SubsonicService(Application* app, InternetModel* parent);
  ~SubsonicService();

  QStandardItem* CreateRootItem();
  void LazyPopulate(QStandardItem *parent);

  static const char* kServiceName;
  static const char* kSettingsGroup;
  static const char* kApiVersion;
  static const char* kApiClientName;

signals:

public slots:

private:
  QStandardItem* root_;
};

#endif // SUBSONICSERVICE_H
