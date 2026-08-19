#include <QApplication>
#include "mainwindow.h" // 引用已有的头文件

int main(int argc, char* argv[]) {
    QApplication a(argc, argv);
    MainWindow w;
    w.show();
    return a.exec();
}