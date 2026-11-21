#pragma once

#include <QIntValidator>
#include <QLineEdit>
#include <QLocale>

class DimensionEdit : public QLineEdit
{
    Q_OBJECT

public:
    static constexpr int minimum = 1;
    static constexpr int maximum = 1000000;

    explicit DimensionEdit(QWidget *parent = nullptr) : QLineEdit(parent)
    {
        auto validator = new QIntValidator(minimum, maximum, this);
        auto numericLocale = QLocale::c();
        numericLocale.setNumberOptions(QLocale::RejectGroupSeparator);
        validator->setLocale(numericLocale);
        setValidator(validator);
        setInputMethodHints(Qt::ImhDigitsOnly);
        connect(this, &QLineEdit::textChanged, this, [this](const QString &text) {
            if (!hasAcceptableInput()) return;
            const int number = text.toInt();
            if (number == currentValue) return;
            currentValue = number;
            emit valueChanged(currentValue);
        });
    }

    int value() const { return currentValue; }

    void setValue(int value)
    {
        const int clipped = qBound(minimum, value, maximum);
        if (signalsBlocked()) currentValue = clipped;
        setText(QString::number(clipped));
    }

signals:
    void valueChanged(int value);

protected:
    void focusOutEvent(QFocusEvent *event) override
    {
        const auto number = QString::number(currentValue);
        if (text() != number) setText(number);
        QLineEdit::focusOutEvent(event);
    }

private:
    int currentValue = minimum;
};
