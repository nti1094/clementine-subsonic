#include "subsonicservice.h"

const char* SubsonicService::kServiceName = "Subsonic";
const char* SubsonicService::kSettingsGroup = "Subsonic";
const char* SubsonicService::kApiVersion = "1.8.0";
const char* SubsonicService::kApiClientName = "Clementine";

SubsonicService::SubsonicService(Application* app, InternetModel* parent)
  : InternetService(kServiceName, app, parent, parent)
{
}

SubsonicService::~SubsonicService()
{

}

QStandardItem* SubsonicService::CreateRootItem()
{
  root_ = new QStandardItem(QIcon(":/providers/subsonic.png"), kServiceName);
  root_->setData(true, InternetModel::Role_CanLazyLoad);
  return root_;
}

void SubsonicService::LazyPopulate(QStandardItem *parent)
{

}
