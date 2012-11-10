#include "subsonicsettingspage.h"
#include "ui_subsonicsettingspage.h"

SubsonicSettingsPage::SubsonicSettingsPage(SettingsDialog* dialog)
  : SettingsPage(dialog),
    ui_(new Ui_SubsonicSettingsPage)
{
  ui_->setupUi(this);
  setWindowIcon(QIcon(":/providers/subsonic-32.png"));
}

SubsonicSettingsPage::~SubsonicSettingsPage()
{
  delete ui_;
}

void SubsonicSettingsPage::Load()
{

}

void SubsonicSettingsPage::Save()
{

}
