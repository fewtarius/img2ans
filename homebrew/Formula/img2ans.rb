# typed: false
# frozen_string_literal: true

# version: 0.0.0-dev  (updated automatically by release pipeline)
class Img2ans < Formula
  desc "Convert images to CP437 ANSI art for telnet BBSs and terminals"
  homepage "https://github.com/fewtarius/img2ans"
  url "https://github.com/fewtarius/img2ans/archive/refs/tags/v0.0.0-dev.tar.gz"
  sha256 "0000000000000000000000000000000000000000000000000000000000000000"
  license "GPL-3.0-or-later"

  depends_on "cmake" => :build

  def install
    system "cmake", "-B", "build",
                    "-DCMAKE_BUILD_TYPE=Release",
                    *std_cmake_args
    system "cmake", "--build", "build"
    system "cmake", "--install", "build"
  end

  test do
    # Generate a minimal test PNG with Python and convert it
    (testpath/"gen.py").write <<~PYTHON
      import struct, zlib, sys

      def png_chunk(name, data):
          crc = zlib.crc32(name + data) & 0xFFFFFFFF
          return struct.pack('>I', len(data)) + name + data + struct.pack('>I', crc)

      w, h = 16, 16
      raw = b''
      for y in range(h):
          raw += b'\x00'
          for x in range(w):
              raw += bytes([int(x/w*255), int(y/h*255), 80])
      compressed = zlib.compress(raw)
      png = (b'\x89PNG\r\n\x1a\n'
             + png_chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
             + png_chunk(b'IDAT', compressed)
             + png_chunk(b'IEND', b''))
      open(sys.argv[1], 'wb').write(png)
    PYTHON
    system "python3", "gen.py", "test.png"
    system bin/"img2ans", "--cols", "20", "--rows", "5", "test.png", "test.ans"
    assert_predicate testpath/"test.ans", :exist?
    assert_operator (testpath/"test.ans").size, :>, 0
  end
end
