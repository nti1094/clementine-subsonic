#ifndef SUBSONICSETTINGSPAGE_H
#define SUBSONICSETTINGSPAGE_H

#include "ui/settingspage.h"

class Ui_SubsonicSettingsPage;

class SubsonicSettingsPage : public SettingsPage
{
  Q_OBJECT

public:
  SubsonicSettingsPage(SettingsDialog* dialog);
  ~SubsonicSettingsPage();

  void Load();
  void Save();

private:
  Ui_SubsonicSettingsPage* ui_;
};

#endif // SUBSONICSETTINGSPAGE_H
