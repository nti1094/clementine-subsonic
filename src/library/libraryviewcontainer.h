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

#ifndef LIBRARYVIEWCONTAINER_H
#define LIBRARYVIEWCONTAINER_H

#include <QWidget>

class LibraryFilterWidget;
class LibraryView;
class Ui_LibraryViewContainer;

class LibraryViewContainer : public QWidget {
  Q_OBJECT

public:
  LibraryViewContainer(QWidget* parent = 0);
  ~LibraryViewContainer();

  LibraryFilterWidget* filter() const;
  LibraryView* view() const;

private:
  Ui_LibraryViewContainer* ui_;
};

#endif // LIBRARYVIEWCONTAINER_H