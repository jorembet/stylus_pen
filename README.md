# Stylus Pen

Aplikasi gambar native Linux dengan dukungan tekanan dan kemiringan stylus. Ditulis C++17 di atas GTK4 dan Cairo, tanpa Electron.

## Menjalankan

```bash
cd ~/apps/stylus-pen-cpp
./run.sh          # build sekali lalu jalankan
```

Sudah terpasang di menu GNOME sebagai "Stylus Pen".

Ikon aplikasi ada di `data/stylus-pen.svg`, dan `.desktop` menunjuknya lewat `Icon=stylus-pen`. Jendela yang sedang berjalan memakai nama yang sama lewat `gtk_window_set_icon_name`, jadi ikon di taskbar sama persis dengan ikon di menu.

GTK mencari nama ikon lewat *icon theme*, yang hanya menelusuri direktori yang terpasang. Kalau tidak dinstall, jendela akan kembali ke ikon generik GTK dan tidak sama dengan menu. Karena itu `main.cpp` mendaftarkan dua direktori ke search path tema: `<repo>/data` untuk `./run.sh`, dan `share/icons/hicolor/scalable/apps` untuk aplikasi yang terinstall. Jadi keduanya cocok tanpa perlu `sudo install`.

## Build manual

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build --parallel
./build/bin/stylus-pen
```

Ketergantungan: `gtk4` dan `cairo`, sudah terpasang di Ubuntu. Kalau belum:

```bash
sudo apt install build-essential cmake libgtk-4-dev libcairo2-dev
```

## Brush

| Brush | Key | Perilaku |
|---|---|---|
| Pen | `B` | Lebar goresan mengikuti tekanan stylus |
| Pencil | `N` | Tekanan memengaruhi lebar dan opacity, plus butiran tekstur |
| Marker | `M` | Lebar hampir konstan, transparansi rendah, blend `multiply` |
| Eraser | `E` | Menghapus per layer, tidak merusak layer di bawahnya |

**Pressure width** mengatur lebar goresan dari tekanan pena. **Tilt shading** memodelkan nib pena yang condong: goresan melebar searah kemiringan dan menyempit tegak lurusnya, seperti ujung pena datar yang menekan permukaan. Keduanya bisa dimatikan di panel **Brush options** di sidebar, bersama **Stabilizer**.

Slider **Pressure level** di panel yang sama mengatur seberapa kuat pena merespons tekanan, dari 0% sampai 100%. Ini bukan saklar on/off seperti **Pressure width**, melainkan besaran, sebab pengaturan yang berguna hampir tidak pernah ada persis di kedua ujung. Pena yang 100% mengikuti tekanan sulit dipakai untuk menggambar garis rata, sedangkan pena yang 0% tidak bisa menyempit sama sekali.

Di 0% semua sampel ditarik ke lebar netral yang sama, bukan ke lebar paling tipis, jadi "mengabaikan tekanan" berarti goresan rata dengan lebar sedang. Level ini juga meredam alpha butiran pencil, supaya butirannya tidak tetap gelap di atas goresan yang sudah tidak bertaper. Nilainya ikut tersimpan di file proyek.

Tekanan pena mentah sedikit bergetar, jadi nilainya dilewatkan filter satu-pole yang responsif naik maupun turun. Goresan yang diratakan tetap mengikuti tangan dalam satu-dua sampel, sementara derau tidak lagi bergerakkan tepi. Untuk mouse dan touch yang tidak punya sumbu tekanan, tekanan diturunkan dari kecepatan: goresan lambat terbaca berat, goresan cepat menjadi tipis.

Layar sentuh dipakai lewat event touch-nya, bukan lewat pointer event yang GDK hasilkan darinya. Alasannya pointer turunan itu milik satu kontak yang dipilih compositor: dengan dua jari turun, ia melaporkan gerakan satu jari dan lepasnya jari yang lain, jadi goresan melompat antara keduanya. Kontak pertama yang menyentuh kanvas yang menggambar; jari berikutnya diabaikan sampai yang pertama diangkat, sehingga telapak tangan yang menyentuh layar di tengah goresan tidak menyeret outline dan tidakakhiri goresan lebih awal. Kontak dicocokkan lewat event sequence, dan sequence yang null diperlakukan sebagai satu jari tanpa nama, bukan sebagai alasan untuk tidak menggambar sama sekali.

Chip di sebelah slider size menggambar jejak nyata dari brush yang aktif pada tekanan saat ini. Tanpa itu, slider cuma angka dan perbedaan nib 3 dengan 4 baru terasa setelah goresan tersimpan di halaman.

## Tekanan pena

Status bar punya **meter tekanan langsung**: sebuah bar plus angka persen, isinya tekanan yang benar-benar dipakai brush.

Angka tekanan sebenarnya sudah ada di label perangkat sebagai teks, tapi hanya muncul saat pena dipakai. Artinya pengguna mouse tidak punya apa pun untuk diamati, padahal goresannya tetap membawa tekanan yang diturunkan dari kecepatan. Sekarang callback-nya menyala untuk semua jenis input, dan nilainya `brushPressure()` — yang persis jadi lebar dan opasitas goresan, jadi pembacaannya cocok dengan tinta di halaman.

Meter membedakan dua keadaan yang berbeda: **belum ada sampel** (angka `--`, track dengan stub tipis) dan **pena sedang menyentuh tanpa ditekan** (track penuh dengan fill nol). Menampilkan track kosong untuk keduanya membuat "belum ada data" terbaca seperti "penanya sedang di atas kertas".

Fill memakai warna tinta brush, jadi meter sekaligus jadi pratinjau warnanya. Masalahnya tinta bawaan `#1b1b1f` di status bar `#23262b` praktis tidak terlihat — justru pada tekanan rendah, di mana pembacaan paling penting. Warnanya karena itu dicampur ke putih hanya secukupnya untuk melewati latar, dengan nada tetap terjaga.

Pembaruan hanya terjadi ketika persentase yang dibulatkan berubah. Event motion datang beruntun, jadi mengulang queue_draw pada nilai yang sama hanya membuang-buang waktu.

## Navigasi

- `Ctrl` + scroll: zoom
- Scroll biasa atau `Alt` + drag: pan
- `0`: fit to screen
- `[` / `]`: kecilkan/besarkan brush
- `Ctrl+Z` / `Ctrl+Shift+Z` (atau `Ctrl+Y`): undo/redo

## Toolbar

Toolbar dua baris. Baris atas adalah alat gambar dan apa yang dipakai untuk menggambarnya: brush, palet, slider size. Baris bawah adalah aksinya terhadap dokumen dan cara melihatnya: Undo sampai PNG, lalu grup zoom di kanan. Semuanya dulu sebaris; begitu tiap tombol membawa ikon juga, satu baris itu butuh sekitar 2000px — sekitar 500px melewati lebar default jendela, dan tombol zoom terdorong keluar dari layar. Jadi barisnya dipisah, bukan dipadatkan.

Semua tombol punya ikon. Ikon untuk aksi file dan zoom diambil dari icon theme (`edit-undo-symbolic`, `document-save-symbolic`, `zoom-in-symbolic`, dan seterusnya), sama seperti ikon mata di panel layer. Empat alat gambar, serta ikon **Move image**, **Fit**, dan **Pen → Text**, digambar sendiri dengan cairo: tidak ada icon theme yang punya satu set pena, pensil, marker, dan penghapus, dan yang paling mendekati "pena" adalah ikon tablet — itu alat, bukan coretan.

Ikon yang digambar bukan gambar tetap. Setiap glyph digambar ulang pada skala layar yang sedang dipakai, jadi tetap tajam di layar HiDPI, dan warnanya diambil dari properti CSS `color` milik widget-nya. Karena itu hover, aktif, dan nonaktif mengubah ikon persis seperti mengubah label di sebelahnya, tanpa kode tambahan untuk tiap status.

Tombol yang hanya ikon tetap diberi nama aksesibel secara eksplisit. `GtkButton` yang anaknya bukan `GtkLabel` tidak melaporkan label apa pun, jadi tanpa itu ketiga tombol zoom sunyi bagi pembaca layar; tooltip bukan gantinya, karena hanya diumumkan sebagian pembaca dan hanya setelah hover.

## Tombol

**Undo** dan **Redo** ada di toolbar, di sebelah tombol brush. Keduanya otomatis nonaktif saat tidak ada yang bisa dibatalkan atau diulang — tombol yang terlihat aktif tapi diam-diam tidak melakukan apa pun lebih buruk daripada nonaktif.

**New page** dan **Reset** ada di sidebar, di bawah bagian PAGE. Keduanya ber-level halaman, bukan gambar:

| Tombol | Yang berubah | Bisa di-undo |
|---|---|---|
| New page | Sheet baru kosong: artwork dibersihkan, warna dan kertas kembali ke bawaan | Ya |
| Reset | Hanya halaman: warna kembali ke off-white, kertas kembali Plain. **Artwork tidak disentuh** | Tidak |

Reset sengaja tidak memakai `newCanvas()`, karena itu akan menghapus artwork — satu-satunya hal yang justru dijanjikannya. Dan tidak punya langkah undo sendiri, sama seperti memilih warna: halaman itu perabot, bukan langkah menggambar.

Keempat tombol ini tidak semuanya muat di toolbar. Semuanya sebaris membuat toolbar melebar sekitar 234px melewati lebar default jendela — masalah yang sudah pernah terjadi di sini dan membuat tombol file terdorong keluar. Jadi Undo/Redo tetap di toolbar, sementara New page dan Reset pindah ke sidebar, yang memang sudah jadi tempat bagian PAGE dan PAPER. Agar muat, label **Export PNG** jadi **PNG** (arti penuhnya ada di tooltip dan ikonnya) dan slider size menyempit dari 130 ke 112.

## Image

Tombol **Image** di toolbar memasukkan gambar ke layer-nya sendiri, dinamai dari nama file, dan langsung jadi layer aktif. Layer khusus inilah yang membuat gambarnya bisa dipindah: layer menggambar di atas layer aktif, jadi gambar punya tempatnya sendiri tanpa harus ikut dengan tinta lain.

Gambar lebih besar dari halaman diperkecil supaya muat, sementara gambar kecil dibiarkan pada ukuran aslinya supaya ikon kecil tidak dibesar-besarkan.

Untuk memindahkannya, nyalakan **Move image** lalu seret di mana saja di canvas. Yang bergerak adalah gambar pada **layer yang sedang terpilih**, bukan gambar yang sedang diinjak kursor: memindahkan harus terasa seperti.handle selected, dan tidak ada yang perlu membidik piksel. Mode ini mati secara default karena menggambar adalah tugas utama. Selama mode menyala tapi layer aktif bukan picture, status bar mengatakannya sendiri — jadi tidak ada yang menyeret ke kekosongan tanpa penjelasan.

Gamma dikoreksi terhadap zoom, jadi gambar menempel ke pointer di setiap tingkat magnifikasi. Satu seretan menjadi **satu langkah undo**, dan seretan yang berakhir di titik awal tidak pushing apa pun.

Bagian **IMAGE** di sidebar muncul hanya kalau layer aktif memang berisi gambar, jadi tidak pernah memakan tempat untuk kontrol yang tidak bisa dipakai. Isinya slider **Size** (persentase, 10–400%), tombol **Fit page**, **Centre**, dan **Delete**.

Mengecilkan dan memperbesar dilakukan **dari titik tengah**, bukan dari sudut kiri atas. Kalau yang dikunci pojok kiri atas, setiap langkah slider membuat gambar meluncur menjauh dari tempat kursor berada.

Slider di-commit saat kehilangan fokus, bukan per langkah — satu sapuan = satu langkah undo. Sapuan yang berakhir di skala awal juga tidak pushing apa pun.

**Fit page** memakai rasio terbesar yang muat, jadi gambar yang lebar tidak dibesarkan melewati halaman hanya karena muat di salah satu sumbu. **Delete** menghapus layer gambarnya — jadi bisa di-undo, dan gambar yang dikembalikan **masih bisa diseret** karena posisinya ikut dibawa dalam snapshot.

Gambar **di-render ke dalam surface layer**, bukan digambar live. Itulah yang membuat `snapshot()`, undo goresan, dan format simpan tidak perlu disentuh — ketiganya hanya membaca surface, dan gambar yang hidup akan tidak terlihat oleh mereka. Sumber aslinya disimpan supaya seretan bisa menaruhnya di tempat lain.

Konsekuensinya satu, dan itu disengaja: layer gambar adalah lapisan picture, jadi memindahkan gambar menulis ulang isi layer itu. Kalau Anda menggambar tambahan di atas gambar, tambahan itu hilang saat digeser.

Undo lalu redo pada perubahan struktural membangun ulang layer dari snapshot. Snapshot itu sudah memuat gambar di posisi yang benar, jadi posisi hanya **diadopsi** sebagai metadata — bukan di-render ulang, karena itu akan menghapus piksel yang baru dipulihkan. Tanpa ini satu putaran undo/redo menghasilkan gambar yang masih terlihat tapi tidak bisa lagi diseret.

## Layer

Tambah, duplikat, hapus, gabungkan ke bawah, ubah visibilitas lewat ikon mata, ganti nama dengan klik ganda. Layer menggambar di atas layer aktif, jadi eraser tidak menembus ke bawah. Panelnya menyediakan: tambah, duplikat, naik, turun, gabung, hapus, ubah visibilitas lewat ikon mata, ganti nama dengan klik ganda, dan ubah opacity dengan klik pada persentase. Tombol yang tidak berlaku untuk layer aktif otomatis nonaktif.

Undo untuk goresan hanya menyimpan piksel di area yang benar-benar berubah, jadi history tetap ringan untuk goresan panjang. Perubahan struktural (tambah/hapus/gabung layer) menyimpan seluruh stack.

Gabungan layer di-cache: setiap frame hanya satu blit terskala, bukan seluruh stack 1600×1100 yang dikomposit ulang. Cache di-invalidate saat piksel layer berubah atau flag/opacity berubah.

## Latar halaman

Warna halaman bisa dipilih lewat tombol **Page** di panel Brush options, yang membuka color dialog sistem. Tombol **Reset** mengembalikan ke off-white bawaan (`#fbfbfd`).

Warna ini milik dokumen, bukan milik layer: layer adalah tinta bening di atas kertas, jadi warna kertas harus tetap ada walaupun semua layer disembunyikan. Layer tidak bisa berperan sebagai kertas — menyembunyikan layer paling bawah akan menjatuhkan halamannya juga, dan PNG export tidak akan punya apa pun untuk dibakar ke dalam gambarnya.

Dipakai di dua tempat sekaligus: canvas melukisnya, dan `flatten()` membakarnya ke PNG export. Keduanya dulu tidak sama — canvas memakai off-white sementara export memakai putih murni, jadi halaman terang diekspor jadi putih. Sekarang keduanya membaca satu sumber, sehingga yang diekspor persis sama dengan yang terlihat.

Garis tepi halaman ikut menyesuaikan: dulu selalu hitam transparan, yang lenyap begitu halaman menjadi gelap dan tepi kertas berhenti memberi tahu di mana kertas berakhir.

Warna halaman tidak punya langkah undo sendiri — warnanya bukan gambar, dan satu entri history per perubahan warna akan memenuhi daftar undo goresan. Tapi warnanya ikut dibawa `captureStack`, jadi undo langkah struktural mengembalikan halaman yang sedang dilihat pengguna. **New canvas** mengembalikan ke warna bawaan, dan `Ctrl+Z` sesudahnya membawa warna lama itu kembali.

## Kertas

Garisan di kertas dipilih lewat deretan chip di panel Brush options, masing-masing chip menggambar pola aslinya sebagai pratinjau, lalu **Spacing** mengatur jarak antar garis dalam piksel dokumen.

| Kertas | Tampilan |
|---|---|
| Plain | Tanpa garisan |
| Ruled | Garis horizontal saja, seperti buku tulis bergaris |
| Grid | Garis horizontal dan vertikal, seperti kertas Bernardo |
| Dotted | Titik di setiap perpotongan, seperti buku tulis titik |
| Checkered | Kotak selang-seling, supaya area tembus-transparan kelihatan |

**Spacing** mengatur jarak antar garis dalam piksel dokumen. Slider-nya **selalu aktif**, termasuk saat halaman masih Plain dan belum ada garis yang bisa diatur jaraknya. Dulu ia dinonaktifkan sampai ada pola yang dipilih — masuk akal di atas kertas, tapi Plain adalah bawaan dan yang dikembalikan **Reset**, jadi kontrolnya terlihat rusak di dokumen baru dan setiap kali reset. Spacing adalah setelan halaman yang bertahan lintas jenis pola, jadi boleh diatur lebih dulu; angkanya sudah siap saat polanya dipilih. Rentang slider (8–128px) memakai konstanta yang sama dengan clamp di model, jadi kendali dan dokumen tidak pernah berbeda pendapat tentang nilai yang sah — kalau tidak, proyek tersimpan dengan pitch lebar akan terbuka dengan slider tertahan di maksimum sementara label menampilkan angka lain.

Garisan ini milik halaman, bukan milik layer: tidak bisa dihapus dengan eraser, dan menyembunyikan semua layer tidak membuatnya hilang. Ia digambar antara warna halaman dan layer stack, jadi selalu di belakang tinta — persis seperti kertas cetak. Di canvas ia digambar di ruang dokumen, jadi ikut zoom bersama artwork, bukan melayang dengan jarak layar tetap.

Pitch-nya dikunci ke atas dan ke bawah. Nilai asalnya bisa dibaca dari file proyek yang bisa diedit pengguna, dan spacing `0.0001` akan membuat `paintPaper` looping sekali per piksel dokumen — hang sekaligus cara membuat aplikasi meminta alokasi tak terbatas.

Chip memakai pitch tetap 9px, bukan spacing dokumen yang diskalakan ke dalam chip 44px: pada 32px, grid dan ruled akan sama-sama muat dalam satu baris dan chip jadi tidak bisa membedakannya — justru hal yang harus dibedakannya.

Tinta garisan dipilih terhadap warna halaman, bukan hitam tetap. Hitam transparan lenyap begitu halaman digelapkan, jadi saat user memilih latar gelap, garisan yang sudah dipilih ikut menghilang diam-diam.

Lebar garis dihitung agar tetap **1 piksel perangkat** saat tampilan dikecilkan. Kalau tidak, sebuah halaman 1600×1100 yang di-fit ke jendela menggambar tiap garis di bawah satu piksel, dan antialiasing menggosong seluruh ruling jadi kabut tipis yang tidak terbaca sebagai kertas. Diperbesar, garis menebal seperti kertas yang didekati — itu yang membuatnya terasa bagian dari lembar, bukan overlay di layar.

Kertas Ruled memakai satu garis margin yang lebih tebal di baris teratas, seperti buku tulis sungguhan. Garis tunggal asimetris itu sebagian besar yang membuat halaman bergaris terbaca sebagai bergaris, bukan sebagai persegi bergaris.

Kind-nya dibawa pada widget-nya sendiri (`g_object_set_data`), bukan ditangkap lambda. Dulu handler-nya mengirim **kind yang sedang aktif di dokumen** — di dokumen baru itu selalu Plain — sehingga setiap chip memanggil `setPaper(Plain, …)` dengan nilai yang sama dengan keadaan sekarang, fungsinya mengembalikan false, dan tidak terjadi apa-apa. Chip tertekan, kanvas tetap polos. Pola yang sama sudah dipakai tombol layer lewat `action-slot`, jadi ini persis idiom yang sudah ada di repo ini.

Karena handler signal GTK tidak punya tes di `test_core` (library itu sengaja tanpa GTK), jalur ini baru ketahuan setelah report user bilang garis tetap tidak muncul — padahal render-nya sendiri sudah benar sejak awal. Menelusuri jalur sinyal lebih dulu adalah langkah yang seharusnya dilakukan lebih awal.

Chip harus menggambar ruling di atas ukuran chip itu sendiri. Pernah ukurannya `lebar / pitch`, jadi sebuah chip 40×28 meminta halaman 7×7 — terlalu kecil untuk satu garis pun, dan kelima chip keluar kotak kosong.

## Menyimpan

Format `.styluspen`: baris header `STYLUSPEN3 <lebar> <tinggi> <jumlah layer>`, lalu metadata (layer aktif, setelan brush, warna latar halaman, jenis dan spacing kertas, dan nama/visibilitas/opacity tiap layer), lalu payload PNG per layer. Setiap payload diawali panjangnya dan diakhiri newline. File versi lama (`STYLUSPEN2` dan `STYLUSPEN`) masih bisa dibaca, dan karena blok metadata berupa aliran ber-tag, file lama yang tidak punya tag `background` tetap terbuka dengan warna latar bawaan. Tidak perlu dependensi JSON.

Level sensitivitas tekanan disimpan sebagai field terakhir di baris `settings`, opsional. Field ini dibaca hanya selama masih ada sisa input di baris itu: pembaca token melompati newline seperti spasi, jadi membaca satu token lagi tanpa penjaga akan mengambil nama tag berikutnya ke slot level. File lama tanpa field ini tetap terbuka dengan respons penuh.

Warna latar disimpan sebagai `STYLUSPEN3`, bukan sebagai tag tambahan di `STYLUSPEN2`: pembaca versi lama akan berhenti di tag `background` yang tidak dikenalanya lalu gagal mengurai payload di belakangnya, jadi menaikkan magic-nya membuat kegagalan itu eksplisit. Nilai yang rusak ditolak saat dibaca, bukan saat dilukis — file yang diedit tangan jatuh ke warna bawaan, bukan ke halaman hitam.

Format ini dulu menulis header dan payload saja — nama layer, visibilitas, opacity, layer aktif, dan seluruh setelan brush hilang saat disimpan, jadi membuka file mengembalikan nama kosong dan opacity 100%. Selain itu payload tidak dipisah newline, sehingga panjang layer kedua menempel pada byte terakhir PNG pertama dan proyek dengan dua layer atau lebih sama sekali tidak bisa dibuka lagi.

**Export PNG** menghasilkan komposit semua layer yang terlihat pada resolusi penuh (1600×1100).

## Verifikasi

```bash
./test.sh
```

Menjalankan 493 assertion unit test (geometri, brush, rendering, layer, history, AppState, kertas halaman, round-trip simpan/muat) plus render PNG tiap mode brush ke `/tmp` untuk inspeksi visual.

`test_core` menguji hal yang tidak terlihat secara visual, termasuk regresi untuk bug yang ditemukan lewat screenshot dan lewat probe cairo terpisah:

- **Undo tidak boleh menghapus piksel lain.** `blitRegion` mengomposit snapshot dengan `CAIRO_OPERATOR_SOURCE` tanpa clip, jadi seluruh piksel di luar region goresan terganti margin transparan snapshot. Satu `Ctrl+Z` menghapus sisa isi layer.
- **Undo di tepi halaman harus kembali tepat ke tempatnya.** Snapshot di-clip ke ukuran layer; memulihkannya di origin yang tidak ter-clip menggeser piksel.
- **Entri undo tidak boleh memegang `Layer*`.** Undo struktural membangun ulang seluruh objek `Layer`, sehingga pointer yang disimpan goresan jadi dangling.
- **Goresan horizontal/vertikal harus punya snapshot.** `boundsOf` mengembalikan rect dengan tinggi atau lebar nol, dan `Rect::empty()` memperlakukannya sebagai "tidak ada" — undo jadi diam-diam tidak melakukan apa pun untuk goresan paling biasa.
- **Ujung goresan harus membulat ke luar.** Sapuan cap searah yang salah memotong takik berbentuk V di kedua ujung.
- **Cap tidak boleh dua lapis tinta.** Diisi terpisah, tinta terkomposit dua kali di area tumpang tindih: ujung marker lebih gelap dari bagian tengahnya, dan eraser meninggalkan sambungan.
- **Outline goresan harus satu poligon tertutup.** `moveTo` yang tersesat saat menelusuri sisi balik akan memecah pita jadi dua strips, dan hasilnya goresan berongga bukan solid.
- **Cusp tidak boleh melipat pita.** Di titik belok, tangen tidak punya arah; memakai normal sembarang menukar dua sisi pita.
- **Tilt harus ikut diinterpolasi saat resample**, bukan disalin dari ujung segmen, supaya orientasi nib tidak melompat tiap sampel.
- **Bounds harus memuat semua tinta yang digambar**, termasuk lebar tambahan dari tilt.
- **Cache composite harus di-invalidate** oleh perubahan flag, opacity, dan piksel layer.
- **Panel layer tidak boleh rebuild saat dipakai.** Rename dan drag opacity dulu ikut memunculkan structural change, jadi panel membangun ulang dirinya sendiri di tengah handler ini — slider atau text entry yang sedang dipakai ikut hancur (use-after-free). Keduanya kini lewat kanal terpisah.
- **Satu drag opacity = satu langkah undo**, bukan satu per sampel.
- **Reorder layer harus benar-benar bisa di-undo.** Versi lama hanya menyimpan indeks aktif, jadi undo mengembalikan seleksi tapi tidak susunan layer.
- **Simpan harus menyimpan nama layer, visibilitas, opacity, layer aktif, dan setelan brush.** Ketiganya hilang total di format lama.
- **Payload PNG harus dipisah newline.** Tanpa itu panjang layer berikutnya menempel pada byte terakhir payload sebelumnya dan proyek multi-layer tidak bisa dibaca.
- **Warna rusak harus ditolak, bukan dilempar.** `std::stoi` pada string non-numerik menjatuhkan seluruh proses; string hex datang dari file proyek yang bisa diedit pengguna.
- **Compositing layer harus mempertahankan warna.** `cairo_mask_surface` hanya membawa alpha, sehingga goresan cyan tergambar abu-abu.
- **Warna latar harus ikut terbakar ke PNG export.** `flatten()` memakai putih tetap sementara canvas melukis off-white, jadi halaman terang diekspor jadi putih dan yang tersimpan tidak sama dengan yang terlihat.
- **Warna latar harus ikut di undo struktural.** `captureStack` hanya menyimpan layer, jadi `Ctrl+Z` setelah menambah layer mengembalikan artwork ke kertas yang berbeda.
- **File versi lama harus tetap terbuka, dan tetap dengan warna bawaan.** Tag `background` baru tidak boleh membuat file `STYLUSPEN2`/`STYLUSPEN` ditolak, dan warnanya yang rusak harus jatuh ke default — bukan ke halaman hitam.
- **Garisan kertas harus ikut terbakar ke PNG export**, dan harus tetap di bawah tinta — kalau digambar setelah layer, ruling akan melompati goresan dan PNG export tidak akan punya sama sekali.
- **Spacing kertas harus dikunci.** Nilai asalnya berasal dari file yang bisa diedit pengguna; spacing `0.0001` membuat `paintPaper` looping sekali per piksel dokumen.
- **Jenis kertas yang rusak harus jatuh ke Plain**, bukan ke pola yang tidak ada.
- **Level sensitivitas tekanan harus ikut tersimpan** dan dibaca opsional: file lama tanpa field itu harus tetap terbuka dengan respons penuh, dan nilai di luar 0..1 harus dijepit saat dibaca.
- **Ruling harus ikut di undo struktural** seperti warna latar, kalau tidak artwork balik ke kertas yang berbeda.
- **Tombol undo/redo harus tepat satu langkah di depan history.** `endStroke` memberi tahu listener tepat setelah menggambar dan *sebelum* mendorong entri, jadi tombol Undo tetap nonaktif sampai ada perubahan lain yang kebetulan datang. Semua push sekarang lewat `pushHistory()`, yang mendorong dulu lalu memberi tahu.
- **Undo dan redo harus mengabar kanal `setChanged`.** Keduanya dulu hanya mengabar kanal layer, jadi tombol Redo tetap nonaktif tepat setelah Undo ditekan — jalan buntu yang tidak terlihat sampai ada perubahan lain.
- **Reset tidak boleh menghapus artwork.** Dulu diarahkan ke `newCanvas()` yang membersihkannya, persis hal yang tidak dijanjikan tombol itu.
- **Menyeret gambar harus benar-benar bergerak.** Cabang hover di `GDK_MOTION_NOTIFY` mengecek "sedang menggambar atau sedang pan" lebih dulu, dan saat seretan gambar keduanya salah -- jadi `break` terjadi sebelum cabang seretan sempat jalan. Hasilnya: tombol ditekan, kursor bergerak, gambar diam. Keputusan dipisah jadi fungsi murni `classifyInput()` supaya bisa diuji tanpa membuat event GDK sintetis, yang memang opaque di GTK4.
- **Alt+drag harus tetap pan walau Move image menyala.** Cabang gambar dicek lebih dulu, jadi pintasan pan mati begitu mode Move dinyalakan.
- **Pilih layer tidak boleh rebuild panel dari dalam handlernya sendiri.** `selectLayer()` dipanggil dari handler `pressed` milik `GtkGesture` pada baris yang sedang ditekan, lalu `refreshLayers()` mencabut dan membangun ulang seluruh baris -- termasuk gesture yang emisi signal-nya masih ada di stack. GTK terus memakai widget itu untuk sisa urutan klik setelah handler kembali, jadi jendela ikut turun. Rebuild sekarang ditunda ke idle. Ini muncul sebagai crash "Move image" padahal bukan: memilih layer gambar adalah langkah tepat sebelum Move image, jadi selectulah yang sedang patah.
- **Mulai seret harus benar-benar dimulai sebelum pointer diklaim.** `beginMoveActiveImage()` dikabaikan return-nya, jadi `movingImage_` bisa aktif sementara AppState tidak punya drag untuk digerakkan -- setiap motion jadi no-op dan release tidak commit apa pun, jadi gesturenya hilang tanpa jejak.
- **Seretan harus bertahan hammering.** `test_canvas_events` menjalankan 200 seretan x 20 langkah, lalu 200 undo dan 200 redo, di bawah ASan/UBSan. Bersih. Jadi separuh AppState dari gerak ini kuat; kalau masih ada crash, ia ada di `Canvas::onRawEvent`, yang tidak bisa diseret tanpa event stylus sungguhan -- `GdkEvent` opaque di GTK4.
- **Handler sidebar harus diputus sebelum window hancur.** GTK memancarkan `notify` saat mem-finalisasi widget, termasuk ketika jendela ditutup. Handler sidebar menangkap `Window*` mentah, jadi notify yang mendarat setelah `~Window()` berjalan menjalankan handler terhadap Window yang sudah dihancurkan -- `state_` dibaca sebagai sampah, `commitImageScale()` membaca flag drag dari memori bebas, lalu `Document::active()` mengindeks vector layer yang sudah bukan miliknya. `~Window()` sekarang menelusuri seluruh pohon widget dan memutus semua handler miliknya sebelum member lain dibongkar. **Catatan jujur:** guard ini tidak pernah berhasil mereproduksi crash yang dituju -- kebocoran itu terlihat dua kali (flag drag dibaca dari memori bebas, vector layer sudah dibongkar), dan mematikan guard ini tidak membuatnya muncul lagi, jadi hubungan antara guard dan crash belum terbukti. Perlakukan sebagai pagar defensif, bukan bukti perbaikan.
- **Dotted harus berupa titik, bukan bidik.** `cairo_arc` menyambung subpath sebelumnya dengan garis lurus, jadi seluruh kisi titik jadi satu poligon terisi dan tergambar sebagai bajang di antar titik. Butuh `cairo_new_sub_path()` per titik.
- **Ruling harus menyatu dengan halaman, bukan menimpanya.** `flatten()` masih di `CAIRO_OPERATOR_SOURCE` setelah mengecat latar, jadi tiap garis melubangi kertas dengan alpha separuh dan PNG export keluar berlubang. Tes piksel sebelumnya hanya melihat kanal warna, tempat piksel hitam transparan terbaca sebagai "gelap", jadi bug-nya lolos.
- **Ruling harus terlihat di halaman gelap**, dan lebarnya harus bertahan di satu piksel perangkat saat dikecilkan.
- **Slider spacing harus bisa dipakai sejak awal.** Nonaktif sampai pola dipilih padahal Plain adalah bawaan, jadi kontrolnya terbaca rusak.
- **Rentang slider dan clamp harus satu sumber.** Keduanya pernah terpisah (slider 96, format 512), jadi proyek berjarak lebar terbuka dengan slider tertahan di maksimum dan label menunjukkan angka lain.
- **Chip kertas harus benar-benar memilih polanya.** Handler-nya mengirim kind yang sedang aktif, jadi setiap chip bermula dari Plain dan tidak pernah mengubah apa pun.
- **Posisi gambar harus ikut dalam undo/redo struktural.** `applyStack` membangun ulang layer dari snapshot; kalau metadata posisi tidak ikut, satu putaran undo/redo menghasilkan gambar yang terlihat tapi tidak bisa lagi diseret.
- **Undo resize harus mengembalikan posisi juga, bukan cuma ukuran.** Kalau hanya skalanya yang dikembalikan, gambarnya meloncat ke pojok halaman.
- **Resize harus dari titik tengah.** Kalau pojok kiri atas yang dikunci, setiap langkah slider membuat gambar meluncur menjauh dari kursor.
- **Objek GTK milik Window harus dilepas.** `GtkColorDialog` dan `GtkCssProvider` di belakangnya tidak pernah di-unref, dan provider harus dilepas dari display dulu — display mengambil referensi sendiri, jadi unref saja meninggalkan display memegang provider yang kelas CSS-nya sudah tidak milik apa pun.
- **Lebar goresan harus benar-benar berubah mengikuti tekanan, bukan konstan.**
- **Jari harus bisa menggambar, dan hanya satu jari yang boleh menggambar.** Pointer event turunan touch harus dibuang, atau dengan dua jari turun goresan melompat antar-kontak dan menyentuh layar mengakhiri goresan yang masih berjalan.

## Kalau stylus tidak terdeteksi

Status bar menampilkan `Stylus active · pressure 0.42 · tilt 31°` saat pena dipakai, dan `Touch active · pressure 0.61 (from speed)` saat layar sentuh dipakai. Kalau tidak berubah dari `No stylus or touchscreen detected`, aplikasi tetap jalan: mouse masih menggambar, dan tekanan untuk semua perangkat tanpa sumbu tekanan diturunkan dari kecepatan nib, jadi goresannya tetap menebal dan menipis.

1. `libinput list-devices` — tablet harus terlihat.
2. `ls /dev/input/event*` — device node harus ada.
3. Modul kernel untuk tablet: `sudo modprobe wacom` (Wacom lama) atau `sudo modprobe hid-wacom`.
4. Di Wayland, compositor harus meneruskan input tablet. GNOME dan KDE Plasma sudah melakukannya.

## Struktur

```
src/color.{h,cpp}       parse/format warna hex, dipakai brush dan latar halaman
src/paper.{h,cpp}       jenis kertas, spacing, tinta kontras, dan penggambaran garisan
src/geometry.{h,cpp}    vektor, rect, simplify, resample, smoothing
src/brush.{h,cpp}       outline goresan varies-width, nib elliptis, butiran pensil
src/document.{h,cpp}    Layer, LayerStack, snapshot undo, cache composite
src/history.{h,cpp}     undo/redo berbasis closure
src/project.{h,cpp}     AppState: stroke, wiring undo, format .styluspen
src/canvas.{h,cpp}      widget GTK4, baca tekanan/tilt dari GdkEvent mentah
src/window.{h,cpp}      toolbar, panel brush + layer, status bar, shortcut
tests/test_core.cpp     493 assertion, tanpa GTK
tests/test_render.cpp   render tiap brush ke PNG
data/stylus-pen.svg     ikon aplikasi, dipakai launcher dan jendela
```

Penekanan dan kemiringan dibaca di `Canvas::sampleFromEvent` dari `gdk_event_get_axis` pada event mentah. Gesture GTK tidak mengekspos nilai sumbu itu, jadi handel-nya membaca `gtk_gesture_get_last_event` lalu `GDK_AXIS_PRESSURE` / `GDK_AXIS_XTILT` / `GDK_AXIS_YTILT` langsung. Jenis pena dikenali lewat `gdk_event_get_device_tool`, yang mengembalikan `GDK_DEVICE_TOOL_TYPE_PEN` dan sejenisnya.
