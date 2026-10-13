#pragma once
#include <QImage>
#include <memory>

namespace comp {
class D3DCanvas {
  public:
    D3DCanvas();
    ~D3DCanvas();
    bool initialize(quintptr window);
    bool present(const QImage& image, bool capture = false);
    QImage readback() const;
    QString backend() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace comp
