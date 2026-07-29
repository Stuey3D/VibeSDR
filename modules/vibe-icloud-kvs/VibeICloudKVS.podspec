Pod::Spec.new do |s|
  s.name         = 'VibeICloudKVS'
  s.version      = '1.0.0'
  s.summary      = 'NSUbiquitousKeyValueStore bridge for VibeSDR iCloud sync.'
  s.homepage     = 'https://github.com/Stuey3D/VibeSDR'
  s.license      = { :type => 'GPLv3' }
  s.author       = 'VibeSDR'
  s.platform     = :ios, '16.4'
  s.source       = { :path => '.' }

  s.source_files = '*.{mm,h}'

  s.dependency 'React-Core'

  s.pod_target_xcconfig = {
    'CLANG_CXX_LANGUAGE_STANDARD' => 'c++17',
    'CLANG_CXX_LIBRARY' => 'libc++',
  }
end
