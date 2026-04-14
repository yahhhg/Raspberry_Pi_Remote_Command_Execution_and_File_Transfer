#include "Host_Computer.h"
#include <QtWidgets/QApplication>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    Host_Computer window;
    window.show();
    return app.exec();
}
