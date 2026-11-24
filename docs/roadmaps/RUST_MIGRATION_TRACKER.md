# Sentinel Rust geçiş takip listesi

Başlangıç kaydı önceki genel geçiş raporunu ve depodaki Rust/Slint kaynaklarını
esas alır. Bu dosya ilerledikçe güncellenir.

- `[x]` İlgili satırın kodu mevcut. Bu işaret üretim doğrulaması anlamına gelmez.
- `[ ]` Kod eksik veya kısmi. Kısmi işlerde mevcut kapsam satırda belirtilir.
- Doğrulama ve üretime alma için alttaki ayrı liste kullanılır.

## AgentRuntime, araçlar ve alt görevler

- [x] Tipli Unix dosya/dizin inceleme, arama, oluşturma ve tam eşleşmeli düzenleme.
- [x] Ortak görev bütçesi, plan ön kontrolü, canlı ilerleme ve oturuma bağlı onay.
- [x] Agent başlangıç, terminal durum ve çökme kurtarmasında sohbet ile denetim kayıtlarının ortak işlemi.
- [x] Unix'te en fazla dört açık salt okunur alt görev, bağımlılık, ayrı onay, üst görev iptali ve denetim kaydı.
- [ ] Genel çok adımlı Agent döngüsü ve araç gözlemlerine göre güvenli plan onarımı.
- [ ] Üretimdeki kalan araçların ortak şema, yetki, sandbox, yürütme ve denetim geçidine taşınması.
- [ ] Kalan izin ve onay politikaları, görev kapsamlı yetki ve iptal edilen izinlerin ortak işlenmesi.
- [ ] Genel döngüde kısmi sonuç, belirsiz yazım, tekrarlanan öneri ve geç gelen sağlayıcı yanıtı yönetimi.
- [ ] Bağımsız alt ajan oturumları ve görev kapsamı. Mevcut alt görevler üst kayıt altında sıralı çalışır.
- [ ] Paralel çocuk yürütme, kotalar, çakışma sıralaması ve çocuk başına iptal.
- [ ] Çocuk sonuçlarının üst görevde kanıtla birleştirilmesi ve yeniden başlatmada ayrı kurtarılması.
- [ ] Slint/TUI'de bağımsız alt ajan listesi, olayları ve denetim ayrıntıları.

## Windows daemon ve Agent

- [x] Named pipe, yerel model keşfi/seçimi, akışlı Chat, iptal ve yeniden bağlanma.
- [x] Profilde sohbet ve model seçimi kaydı. Yarım kalan sohbet yeniden oynatılmadan işaretlenir.
- [x] Verilen metinle sınırlı Agent analizi ve kaynak alıntılarının kontrolü.
- [x] Kapalı Windows geliştirme profilini mevcut kilit altında değiştirmeden inceleyen `--check-windows-profile` komutu. Model kaydı ve Chat şeması doğrulanır; içerik yerine sayılar raporlanır.
- [x] Kapalı Windows geliştirme profilinden doğrulanmış Chat ve model bağının ayrı sahip erişimli dizine tek arşiv olarak, üzerine yazmadan aktarılması.
- [x] Windows geliştirme arşivinden Chat ve model kaydını doğrulayıp yeni özel profil dizinine tek adımda yayımlayarak geri yükleme. Üretim ve platformlar arası yedek uyumluluğu ayrı kapsam.
- [ ] Windows Agent'ın ortak genel AgentRuntime'a bağlanması.
- [ ] Windows çalışma alanı dosya geçidi ve reparse point, yol kimliği, bağlantı denetimi.
- [ ] Windows araç onayı, denetim kaydı, süreç/sandbox ve alt süreç yaşam döngüsü.
- [ ] Üretim profil konumu, ortak yedek sözleşmesi, servis ve tek örnek sahipliği.

## Sohbet, sağlayıcı ve model yönetimi

- [x] Yerel katalog ve sağlayıcı seçimi. OpenAI, Groq, Mistral, Claude ve Gemini için bulut metin yolu.
- [x] Yerel PNG/JPEG/statik WebP, UTF-8/FODT/DOCX/metin PDF ve isteğe bağlı bulut belge metni.
- [x] İsteğe bağlı bulut metin geçmişi. Aynı sağlayıcı ve modelle tamamlanan uygun turlarla sınırlı.
- [x] Rust daemon, Ollama model silmeden önce IPC'de gönderilen model adı ve onay metninin birebir eşleşmesini zorunlu tutar. Slint onay alanını gönderir.
- [x] OpenAI için açıkça seçilen görsel model, bağlama bağlı görsel token sınırı ve mevcut mesaja tek görsel ekleme.
- [ ] Diğer bulut sağlayıcılarda görseller ve görsel yeteneği keşfi.
- [ ] Bulut düzenleme, yeniden deneme ve dallanma. Önceki içeriğin yeniden gönderilmesi için bağlam politikası.
- [x] DeepSeek sabit HTTPS uç noktası, model kataloğu ve düşünme kapalı akışlı metin sohbeti.
- [ ] Kalan üretim sağlayıcı ve protokol kapsamı.
- [x] Belirsiz Ollama işlemi sonrası katalog yenilemesi, tam model kimliğinin varlığını gösterir.
- [ ] Model yükleme/boşaltma, sunucu sahipliği ve Ollama uzak işleminin kesin sonucunu uzlaştırma.
- [ ] Platformlar arası kimlik bilgisi ekleme, değiştirme, kaldırma ve hesap eşlemesi. macOS Keychain ve özel dosyadan okuma mevcut.
- [ ] OCR, kaynak/atıf sunumu ve kalan üretim medya türleri.

## Profil, bellek ve geri dönüş

- [x] Kalıcı bağımsız Rust profili, Rust yedeği ve yeni hedefe geri yükleme.
- [x] Qt aktarımı ön kontrolü. Desteklenen sohbet, ek, hata kategorisi, arşiv/sabitleme, oturum belleği ve ayarlar taşınır.
- [ ] Qt Agent denetimi, kalan ayarlar/bellek, çalışma alanları ve izinler için alan eşlemesi.
- [x] Qt ön aktarımı, aynı özel dizindeki üç kaynak kopyanın kimlik ve SHA-256 değerini başlangıçta ve yayımlamadan önce karşılaştırır.
- [ ] Qt profilinin tutarlı çoklu veri deposu anlık görüntüsü ve WAL kapsamı.
- [ ] Referans ve kayıp envanteri. Desteklenmeyen kayıtlar ve yeniden üretilecek indeksler için açık sonuç.
- [ ] İzinleri güvenli yeniden onaylama ve kimlik bilgilerini OS kasasına yeniden bağlama.
- [ ] Qt/Rust üretim profilinde tek yazıcı devri, yarım aktarım ve yeniden deneme.
- [ ] Rust'ta sonradan oluşturulan verinin Qt'ye geri aktarılması.
- [ ] Oturum dışı bellek, arama/indeks, saklama ve model bağlamına kontrollü geri çağırma.

## Masaüstü, Quick Panel, CLI ve TUI

- [x] Slint masaüstü ile Quick Panel arasında oturum, taslak ve çalışma korunarak geçiş.
- [x] macOS geliştirme kısayolu, menü çubuğu ve gizli başlangıç yolu.
- [ ] Linux Wayland portal kısayolu, aktivasyon/odak ve tepsi. Gereken X11 davranışı.
- [ ] Windows global kısayol, odak, tepsi ve yerel uygulama yaşam döngüsü.
- [ ] Qt'de kalan ekranlar, yerel dosya/medya seçicileri ve eksik ayarlar.
- [ ] Eksik klavye, ekran okuyucu, yüksek DPI, tema ve hata/kurtarma durumları.
- [ ] Eski C++ CLI komutları, betik girişleri, çıktı/çıkış kodu eşdeğerliği ve emeklilik.

## Ses, otomasyon, eklentiler ve kaynaklar

- [x] Sınırlı macOS Rust konuşma tanıma yolu ve salt okunur Rust eklenti kataloğu.
- [ ] Yerel mikrofon, cihaz seçimi ve kaybı, izin, iptal, önizleme ve sesli yanıt.
- [ ] Kullanıcının etkinleştirdiği tetikleyiciler ve zamanlayıcı.
- [ ] Otomasyonda kalıcı kimlik, tekrar ayıklama, çevrimdışı davranış, iptal ve ortak denetim.
- [ ] Eklenti/MCP çalışma zamanı, araç kaydı, izin, izolasyon, çökme ve kaldırma.
- [ ] Donanım uyumu, RAM/VRAM ve disk bütçesi, yükleme/boşaltma ve eşzamanlı istek politikası.

## Paketleme ve üretim

- [x] Qt'siz Linux/macOS aday iş akışı ve macOS arm64 aday paketinin yerel paket doğrulaması.
- [ ] Yerel Linux paketleri, macOS üretim paketi ve Windows kurucusu.
- [ ] Paket imzası, güvenilen anahtarlar, güvenli güncelleme ve paket geri dönüşü.
- [ ] Üretim giriş noktaları, servis ve profil devri.
- [ ] Qt/C++ üretim daemon'u, QML arayüzü, eski CLI ve Qt paketleme girişlerinin emekliye ayrılması.

## Ayrı faz: doğrulama ve yayın

- [ ] Aynı aday sürümün Linux/macOS/Windows derlemeleri ve CI.
- [ ] Agent onay, iptal, tekrar, çökme, kısmi yazım ve denetim tutarlılığı.
- [ ] Canlı yerel/bulut sağlayıcılar, model işlemleri ve ek matrisi.
- [ ] Gerçekçi profil kopyalarıyla devir, kesinti, tekrar ve geri dönüş.
- [ ] Yerel Quick Panel, odak, erişilebilirlik, tepsi ve IPC güvenliği.
- [ ] Karşılaştırılabilir Qt/Rust paketleriyle kaynak tüketimi ve gecikme ölçümü.
- [ ] İmzalı temiz kurulum, yükseltme, kaldırma ve geri dönüş.
- [ ] Rust üretim yayını ve eski hattın devreden çıkarılması.

Başvuru: [Rust geçiş kararı](../decisions/ADR-0005-rust-desktop-runtime-migration.md),
[modül kalite matrisi](MODULE_QUALITY_MATRIX.md),
[kabul matrisi](ACCEPTANCE_MATRIX.md).
