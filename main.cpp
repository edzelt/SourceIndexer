// === main.cpp ===
#include "IndexProgressDialog.h"
#include <QApplication>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    app.setApplicationName("Sourcebreaker Indexer");
    app.setApplicationVersion("0.1");

    IndexProgressDialog wnd;
    wnd.show();

    return app.exec();
}