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
- [x] Sıralı Unix alt görevleri için kalıcı durum geçişlerinden canlı IPC olayları, Slint alt görev seçimi ve TUI `/agent-audit RUN_ID` görünümü.
- [x] Tek alt görev kimliğiyle oturum kapsamlı denetim. Yalnızca o görevin adımları, araçları, izinleri ve kanıtları gösterilir.
- [x] Sağlayıcı sınıflandırma ve karar yanıtı döndüğünde, plan ayrıştırılmadan veya araç isteği işlenmeden önce iptal denetimi.
- [ ] Slint/TUI'de bağımsız alt ajan oturumları ve çocuk başına iptal denetimi.

P1 denetim kabul ölçütü: iki alt görevden birini açınca kardeş görevin araç
ve kanıt kayıtları görünmez. Bağlanılmamış oturum veya olmayan alt görev
reddedilir. Yeniden açılan profilde durum kalıcı kayıttan okunur. Sağlayıcı
yanıtı iptalden sonra geldiğinde karar ayrıştırılmaz ve yeni araç isteği
yayımlanmaz. Alt görevler üst oturuma bağlıdır ve aynı yetki sınırlarını
kullanır.

## Windows daemon ve Agent

- [x] Named pipe, yerel model keşfi/seçimi, akışlı Chat, iptal ve yeniden bağlanma.
- [x] Profilde sohbet ve model seçimi kaydı. Yarım kalan sohbet yeniden oynatılmadan işaretlenir.
- [x] Verilen metinle sınırlı Agent analizi ve kaynak alıntılarının kontrolü.
- [x] Windows daemon üzerinde, bağlantının eriştiği oturumlarla sınırlı Agent geçmişi listesi, sayfalama ve çalışma ayrıntısı.
- [x] Kapalı Windows geliştirme profilini mevcut kilit altında değiştirmeden inceleyen `--check-windows-profile` komutu. Model kaydı ve Chat şeması doğrulanır; içerik yerine sayılar raporlanır.
- [x] Kapalı Windows geliştirme profilinden doğrulanmış Chat ve model bağının ayrı sahip erişimli dizine tek arşiv olarak, üzerine yazmadan aktarılması.
- [x] Windows geliştirme arşivinden Chat ve model kaydını doğrulayıp yeni özel profil dizinine tek adımda yayımlayarak geri yükleme. Üretim ve platformlar arası yedek uyumluluğu ayrı kapsam.
- [x] Windows sohbet arşivi ve geri alma. Arşivli oturum yeni Chat veya Agent çalışması kabul etmez.
- [x] Kalıcı Windows sohbet sabitleme ve listede önceliklendirme.
- [x] Kapalı Windows profil kontrolünde arşivli ve sabitlenmiş sohbet sayıları.
- [ ] Windows Agent'ın ortak genel AgentRuntime'a bağlanması.
- [ ] Windows çalışma alanı dosya geçidi ve reparse point, yol kimliği, bağlantı denetimi.
- [ ] Windows araç onayı, denetim kaydı, süreç/sandbox ve alt süreç yaşam döngüsü.
- [ ] Üretim profil konumu, ortak yedek sözleşmesi, servis ve tek örnek sahipliği.

Windows geliştirme profili v1/v2 kayıtlarını okuyup v3 olarak kaydeder. Eski
daemon sürümüne dönüş için yükseltme öncesi profil arşivi gerekir. Kabul
ölçütü, kapalı profil incelemesinin diski değiştirmemesi, bayrakların yeniden
başlatma ve dışa aktarma/geri yükleme sonrasında korunması ve arşivli
oturumda yeni çalışmanın başlamamasıdır.

## Sohbet, sağlayıcı ve model yönetimi

- [x] Yerel katalog ve sağlayıcı seçimi. OpenAI, Groq, Mistral, Claude ve Gemini için bulut metin yolu.
- [x] Yerel PNG/JPEG/statik WebP, UTF-8/FODT/DOCX/metin PDF ve isteğe bağlı bulut belge metni.
- [x] İsteğe bağlı bulut metin geçmişi. Aynı sağlayıcı ve modelle tamamlanan uygun turlarla sınırlı.
- [x] Rust daemon, Ollama model silmeden önce IPC'de gönderilen model adı ve onay metninin birebir eşleşmesini zorunlu tutar. Slint onay alanını gönderir.
- [x] OpenAI, Claude ve Gemini için açıkça seçilen görsel model, bağlama bağlı görsel token sınırı ve mevcut mesaja tek görsel ekleme.
- [x] Slint ek biçimi bilgisinin, seçili bulut modelinin görsel yeteneğine göre model seçiminde ve katalog yenilemesinde güncellenmesi.
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
- [x] Ayrılmış SQLite ana dosya ve WAL kopyalarını isteğe bağlı olarak geçici dizinde birlikte işleme. Eksik veya tutarsız WAL reddedilir.
- [x] Ön kontrolden yayımlamaya kaynak kimliği doğrulaması. `--expect-source-id` farklı kopyayı yayın öncesinde reddeder.
- [x] Linux/macOS üzerinde güncel Qt daemon, masaüstü ve CLI süreçleri ile Rust devir komutu aynı dizin kilidine katılır. Çalışan Qt yazıcısı devri engeller.
- [x] Aynı dizinde bulunan sohbet, ayar ve oturum belleği kopyalarının ortak kilit altında ön kontrolü ve yeni Rust hedefinin yayımlanması. Kaynak kimliği onayı, kalıcı Qt yazma rezervasyonu ve yayımlanmamış devri iptal etme kodu mevcut.
- [ ] Üretim devrinde kalan Qt veri sahipleri, bölünmüş kaynak dizinleri, eski Qt ikilileri, Windows ve başarılı yayın sonrası tam geri dönüş.
- [x] Qt ön aktarımı için `--report-json` ile sohbet, ek, yanıt/değiştirme referansı ve oturum belleği sayımları. Atlanan ayar alanı sayısı ile incelenmeyen alanlar raporlanır.
- [x] Qt ön aktarımında aşama dosyalarının ve dizininin yayımlamadan önce diske yazıldığının onaylanması. SQLite yan dosyaları görülürse yayın durur.
- [x] Qt bellek kopyasında taşınmayan legacy, proje, kişisel ve çalışma alanına bağlı oturum kayıtlarının sayımı. Kaynak arama indekslerinin varlığı raporlanır. Yayımlama için açık seçenek ve ön kontrol kaynak kimliği gerekir.
- [ ] Tam kaynak profil için diğer sahiplerin kayıt düzeyinde kayıp ve indeks envanteri.
- [x] Ayrılmış Qt izin veritabanında yeniden onay gerektiren izinlerin ve eklenti izinlerinin sayımı. İzin veya kaynak yolu taşınmaz.
- [x] macOS’ta seçilen sağlayıcı ve hesabın Rust Keychain kaydına çevrimdışı erişim ön kontrolü.
- [ ] Eski izinlerin tek tek yeniden onaylanması ve sağlayıcı hesaplarının platform kasalarına aktarılması.
- [ ] Qt/Rust üretim profilinde tek yazıcı devri, yarım aktarım ve yeniden deneme.
- [x] Kapalı Rust profilinden yeni Qt v6 sohbet kopyası üretme. Desteklenen sohbet ve mesajlar taşınır. Rust kimlikleri deterministik Qt UUID’lerine eşlenir ve desteklenmeyen veri yayın öncesinde hata verir.
- [x] Ters aktarımda isteğe bağlı Qt JSON ayar kopyası. Tema ve sohbet yanıt talimatları aynı özel hedefte yayımlanır.
- [x] Ters aktarımda isteğe bağlı Qt v7 oturum belleği kopyası. Bellek sahipleri dışa aktarılan sohbet kimlikleriyle eşlenir ve kopya yayın öncesinde doğrulanır.
- [ ] Qt üretim profiline geri yükleme ve desteklenen sohbet/ayar/oturum belleği dışında kalan veri sahiplerinin geri aktarımı.
- [ ] Oturum dışı bellek, arama/indeks, saklama ve model bağlamına kontrollü geri çağırma.

## Masaüstü, Quick Panel, CLI ve TUI

- [x] Slint masaüstü ile Quick Panel arasında oturum, taslak ve çalışma korunarak geçiş.
- [x] macOS geliştirme kısayolu, menü çubuğu ve gizli başlangıç yolu.
- [x] Linux X11 geliştirme Quick Panel'i için Ctrl+Alt+S, gizli başlangıç ve pencere yöneticisine ön plan isteği.
- [x] Wayland Quick Panel kısayolunun kullanıcı oturum portalında oluşturulması, onaylı bağın doğrulanması ve ilgili aktivasyon olayının paneli açıp kapatması.
- [x] Portalın sağladığı Wayland aktivasyon belirtecinin Slint yüzeyinde kullanılması.
- [x] Linux Quick Panel için StatusNotifierItem tepsi simgesi, tıklamayla göster/gizle ve tepsi sunucusu yeniden başladığında kayıt yenileme.
- [ ] Linux tepsi menüsü, paket entegrasyonu ve üretim masaüstü kabulü.
- [x] Windows geliştirme Quick Panel'i için Ctrl+Alt+S, gizli başlatma ve uygulama kapanışında kısayolu serbest bırakma.
- [x] Windows Quick Panel kısayolu küçültülmüş pencereyi geri açar ve Windows'tan ön plana almasını ister.
- [x] Windows Quick Panel tepsi simgesi, göster/gizle, çıkış ve Explorer yeniden başlayınca simgeyi geri kaydetme.
- [ ] Windows üretim uygulaması ve paket yaşam döngüsü.
- [x] Slint Dashboard içinde daemon sağlayıcı/model hazırlığı, çalışma alanı durumu ve çevrimdışı görünüm.
- [x] Slint geniş masaüstünde kalıcı gezinme ve sohbet geçmişi kenar çubuğu. Dar görünüm ve Quick Panel ayrı yerleşim kullanır.
- [ ] Qt'de kalan ekranlar, yerel dosya/medya seçicileri ve eksik ayarlar.
- [x] Slint'te F5 ile bağlantı kurtarma ve F6 ile yazma alanına odaklanma. Dosya listesi yenilenince yalnızca aynı oturum ve çalışma alanında yeniden doğrulanan ekler korunur.
- [ ] Kalan klavye, ekran okuyucu, yüksek DPI, tema ve hata/kurtarma durumları.
- [x] Rust CLI ile boş sohbet oluşturma ve kayıtlı mesajları okuma.
- [x] Rust CLI ile sohbeti yeniden adlandırma ve hedef kimliğini tekrarlayarak kalıcı silme.
- [x] Rust CLI ile sohbeti arşivleme/geri alma ve sabitleme/çözme. Rust Unix ve Windows daemon'larında mevcut.
- [x] Rust CLI sohbet listesinde aktif/arşivli/sabitlenmiş filtreleri.
- [x] Rust CLI ile sohbet ve son çalışma durumunun okunması.
- [x] Rust CLI üzerinden açık çalışma kimliğiyle iptal isteği ve daemon onayı.
X11 kabul ölçütü: Ctrl+Alt+S gizli paneli açar, açık paneli gizler ve çıkışta
kısayol serbest kalır. Meşgul kısayol kayıt hatası verir. Wayland oturumu
XWayland üzerinden küresel kısayol denemez. Pencere yöneticisinin ön plan
kararı yerel doğrulama kapsamındadır.

- [x] Rust CLI üzerinden daemon yönetimli model listeleme, Ollama indirmesi başlatma ve durum sorgulama, desteklenen ayar okuma/yazma.
- [ ] Eski C++ CLI komutları, betik girişleri, çıktı/çıkış kodu eşdeğerliği ve emeklilik.

AC-P6-01 oturum komutları için kabul ölçütü: geçici profilde oluşturulan boş
oturumun kimliği listede görünür ve geçmişi boştur. Ad, arşiv ve sabitleme
değişiklikleri yeniden bağlanınca listede görünür. Yanlış silme onayı bağlantı
kurmadan reddedilir. Doğru onayla işlem tamamlanınca kimlik listeden çıkar ve
geçmiş sorgusu başarısız olur. Destek ilanı bulunmazsa değişiklik isteği
gönderilmez. JSON çıktı modları yalnızca yapılandırılmış veri döndürür.

## Ses, otomasyon, eklentiler ve kaynaklar

- [x] Sınırlı macOS Rust konuşma tanıma yolu ve salt okunur Rust eklenti kataloğu.
- [ ] Yerel mikrofon, cihaz seçimi ve kaybı, izin, iptal, önizleme ve sesli yanıt.
- [ ] Kullanıcının etkinleştirdiği tetikleyiciler ve zamanlayıcı.
- [ ] Otomasyonda kalıcı kimlik, tekrar ayıklama, çevrimdışı davranış, iptal ve ortak denetim.
- [ ] Eklenti/MCP çalışma zamanı, araç kaydı, izin, izolasyon, çökme ve kaldırma.
- [ ] Donanım uyumu, RAM/VRAM ve disk bütçesi, yükleme/boşaltma ve eşzamanlı istek politikası.

## Paketleme ve üretim

- [x] Qt'siz Linux/macOS aday iş akışı ve macOS arm64 aday paketinin yerel paket doğrulaması.
- [x] Windows x64 için Qt'siz Rust masaüstü adayını üretip PE mimarisi, doğrudan Qt bağımlılıkları ve dosya özetleriyle doğrulayan PowerShell hazırlama yolu ve manuel CI işi.
- [ ] Yerel Linux paketleri, macOS üretim paketi ve Windows kurucusu.
- [ ] Paket imzası, güvenilen anahtarlar, güvenli güncelleme ve paket geri dönüşü.
- [x] Qt keşfi ve C++ hedefleri olmadan Rust adayını yapılandıran CMake girişi. Aday CI aynı girişi kullanır. macOS arm64 aday paketi bu girişten derlenip doğrulandı.
- [x] Aday paketindeki Core Warden çalışma zamanı dosyalarının kaynak ağacındaki dosyalarla SHA-256 karşılaştırması.
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
