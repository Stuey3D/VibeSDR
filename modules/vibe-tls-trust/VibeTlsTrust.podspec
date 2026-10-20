Pod::Spec.new do |s|
  s.name         = 'VibeTlsTrust'
  s.version      = '1.0.0'
  s.summary      = 'Per-server certificate pins (trust on first use) for VibeSDR — GitHub #52.'
  s.homepage     = 'https://github.com/Stuey3D/VibeSDR'
  s.license      = { :type => 'GPLv3' }
  s.author       = 'VibeSDR'
  s.platform     = :ios, '16.4'
  s.source       = { :path => '.' }

  s.source_files = '*.{h,m,mm}'
  s.public_header_files = 'VibeTlsCore.h'
  s.frameworks   = 'Security'

  s.dependency 'React-Core'

  s.pod_target_xcconfig = {
    'CLANG_CXX_LANGUAGE_STANDARD' => 'c++17',
    'CLANG_CXX_LIBRARY' => 'libc++',
  }
end
