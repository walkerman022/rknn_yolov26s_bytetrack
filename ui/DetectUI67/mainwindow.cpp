#include "mainwindow.h"
#include <QPushButton>
#include <QDebug>

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    // 在这里添加你的按钮逻辑
    QPushButton* btn = new QPushButton("点击测试", this);
    btn->resize(120, 40);
    btn->move(50, 50);

    connect(btn, &QPushButton::clicked, [=]() {
        qDebug() << "按钮被点击！";
        });
}

MainWindow::~MainWindow() {}