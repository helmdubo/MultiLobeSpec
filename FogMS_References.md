# FogMS — Литература и референсы (для общего ознакомления)

**Кому:** агенту-исполнителю. **Статус:** справочник, не задание. Ничего из списка не обязательно читать целиком — бери по этапу, на котором работаешь.

Правила пользования:
- **Источник истины о движке — исходники UE 5.8.2 и аудит**, не статьи. Статьи объясняют «почему так», код отвечает «как на самом деле».
- Формулы и константы из статей в шейдер **не переносить напрямую**: у всех свои единицы и допущения. Спецификацию даёт исследователь (заметка §3–§4).
- Код из чужих репозиториев не копировать без проверки лицензии. SSFS Карпухина — коммерческий продукт: читаем только публичную документацию, внутренности не восстанавливаем.
- Пометка **[URL не проверен]** — ссылку я не открывал, искать по названию.

---

## 1. Минимум перед A1 (термины, единицы, архитектура froxel-тумана)

**PBR Book, 4-е изд. — Volume Scattering и Equation of Transfer.**
Словарь проекта: поглощение σa, рассеяние σs, ослабление σt, альбедо, фазовая функция, source term, пропускание. Наши документы пишутся в этих терминах.
- https://pbr-book.org/4ed/Volume_Scattering/Volume_Scattering_Processes
- https://pbr-book.org/4ed/Volume_Scattering/Phase_Functions
- https://pbr-book.org/4ed/Light_Transport_II_Volume_Rendering/The_Equation_of_Transfer

**Bart Wronski — «Volumetric Fog: Unified compute shader based solution to atmospheric scattering», SIGGRAPH 2014.**
Исходная архитектура froxel-тумана: плотность → освещение объёма → интегрирование → применение. Volumetric Fog в UE устроен по этой схеме; после слайдов граф проходов из аудита §1 читается как знакомая вещь.
- https://bartwronski.com/wp-content/uploads/2014/08/bwronski_volumetric_fog_siggraph2014.pdf

**Sébastien Hillaire — «Physically Based and Unified Volumetric Rendering in Frostbite», SIGGRAPH 2015 (Advances in Real-Time Rendering).**
Энергосохраняющее интегрирование шага `(S − S·T)/σt` — ровно то, что стоит в `FinalIntegrationCS` (аудит §2.1). Там же — объём extinction и тени от самой среды: идея, которую мы реализуем в A1.
- https://advances.realtimerendering.com/s2015/

**Документация Epic: Volumetric Fog, Local Fog Volumes.**
Что заявлено пользователю, какие ограничения Epic признаёт сам (самозатенение — в списке неподдерживаемого).
- https://dev.epicgames.com/documentation/en-us/unreal-engine/volumetric-fog-in-unreal-engine
- https://dev.epicgames.com/documentation/en-us/unreal-engine/local-fog-volumes-in-unreal-engine

---

## 2. Для A1b — октавы (дешёвая аппроксимация MS)

**Wrenninge, Kulla, Lundqvist — «Oz: The Great and Volumetric», SIGGRAPH 2013 Talks.**
Первоисточник «октав»: несколько копий однократного рассеяния с ослабленным extinction, уменьшенным вкладом и сглаженной фазой. На него ссылается комментарий в `VolumetricCloud.usf:339`. Авторы сами называют это арт-приближением.
- https://history.siggraph.org/learning/oz-the-great-and-volumetric-by-wrenninge-kulla-and-lundqvist

**Wrenninge — «Art-Directable Multiple Volumetric Scattering», SIGGRAPH 2015 Talks.**
Продолжение: зачем плотной белой среде нужны высокие порядки (до сотни отскоков) и как управлять этим художественно.
- https://history.siggraph.org/learning/art-directable-multiple-volumetric-scattering-by-wrenninge

**Hillaire — «Physically Based Sky, Atmosphere and Cloud Rendering in Frostbite», SIGGRAPH 2016 (course notes).**
Октавы в realtime-облаках, роль самозатенения, приложение C — аналитическое энергосохраняющее интегрирование. Лучший текст, чтобы понять, откуда берётся «плотность» облака на референсах заказчика.
- https://media.contentapi.ea.com/content/dam/eacom/frostbite/files/s2016_pbs_frostbite_sky_clouds.pdf
- страница доклада: https://www.ea.com/frostbite/news/physically-based-sky-atmosphere-and-cloud-rendering

**Hillaire — слайды SIGGRAPH 2020, Physically Based Shading course (атмосфера и облака в Unreal Engine).**
Как те же идеи реализованы уже в UE: небо, облака, объёмные тени облаков.
- https://blog.selfshadow.com/publications/s2020-shading-course/hillaire/s2020_pbs_hillaire_slides.pdf

**Исходники UE:** `Shaders/Private/VolumetricCloud.usf` (октавы: 339–430, 1359) и `ParticipatingMediaCommon.ush` — эталон конвенций Epic (аудит §9.1). Документация компонента:
- https://dev.epicgames.com/documentation/en-us/unreal-engine/volumetric-cloud-component-in-unreal-engine

---

## 3. Для A2 / B — пространственный перенос рассеянного света

**Billeter, Sintorn, Assarsson — «Real-Time Multiple Scattering using Light Propagation Volumes», I3D 2012.**
Ближайший аналог нашей цели: объём инициализируется однократным рассеянием, дальше свет распространяется по сетке. Ограничения авторов: изотропная однородная среда. Читать первой в этом разделе.
- https://research.chalmers.se/en/publication/156140
- PDF: https://www.cse.chalmers.se/~uffe/multi_scatter.pdf

**Elek, Ritschel, Dachsbacher, Seidel — «Principal-Ordinates Propagation for Real-Time Rendering of Participating Media», 2014.**
Разделение раннего направленного рассеяния и позднего почти изотропного остатка. Буквально не переносим (у них сетки ориентированы по источникам), но идея разделения — основа нашего будущего режима.
- https://elek.pub/projects/GI2014/index.html

**Hillaire — «A Scalable and Production Ready Sky and Atmosphere Rendering Technique», EGSR 2020.**
Сворачивание бесконечных порядков в геометрический ряд `L₂/(1 − f)`. Важно понимать границы: формула суммирует порядки при изотропном локально-однородном допущении, но **не сообщает, как свет обходит препятствия**. По данным внешнего ревью (мной не проверено), в опубликованном авторском коде есть предупреждение о расходимости при неточном интегрировании — полезный урок про устойчивость; код: репозиторий sebh/UnrealEngineSkyAtmosphere **[URL не проверен]**.
- https://diglib.eg.org/handle/10.1111/cgf14050

**Narasimhan, Nayar — «Shedding Light on the Weather», CVPR 2003.**
Аналитическая модель ореола от точечного источника в однородной среде (APSF) с учётом многократного рассеяния. Кандидат на дешёвый режим для ламп вместо октав.
- https://cave.cs.columbia.edu/old/publications/pdfs/Narasimhan_CVPR03.pdf

**Jos Stam — «Multiple Scattering as a Diffusion Process», 1995.**
Почему в оптически толстой среде перенос ведёт себя как диффузия. Фон для понимания, откуда берётся мягкое заполнение теней; для реализации не нужен.
- https://research.autodesk.com/app/uploads/2023/03/multiple-scattering-as-a.pdf_recqmPkBE5ypFr5Dw.pdf

**Kaplanyan, Dachsbacher — «Cascaded Light Propagation Volumes for Real-Time Indirect Illumination», I3D 2010.** Базовая техника LPV, на которой стоит Billeter. **[URL не проверен]**

**Koerner и др. — «Flux-Limited Diffusion for Multiple Scattering in Participating Media», 2014.** Диффузия, которая не ломается в оптически тонких областях. Дальнее чтение. **[URL не проверен]**

---

## 4. Экранные методы — контекст, НЕ наша цель

Читать, чтобы понимать, чем мы **не** занимаемся и почему заказчика не устроил штатный FSSS.

**Elek, Ritschel, Seidel — «Real-Time Screen-Space Scattering in Homogeneous Environments», 2013.** Физически мотивированная PSF + иерархическая свёртка по mip-пирамиде. FSSS в UE 5.8 считает ширину ядра по этой линии работ (аудит §7).
- https://ieeexplore.ieee.org/document/6449233/
- страница автора: https://elek.pub/research.html

**Tomáš Iser — «Real-time Light Transport in Analytically Integrable Quasi-heterogeneous Media», CESCG 2018.** Расширение на неоднородную среду. Ограничения (из самой работы): оптически тонкая среда, гладкая аналитическая плотность, нет объёмных перекрытий, теряются источники за кадром.
- https://elek.pub/projects/CESCG2018/Iser2018.pdf

**Premože и др. — «Practical Rendering of Multiple Scattering Effects in Participating Media», 2004.** Multiple scattering через point-spread function среды.
- https://diglib.eg.org/items/4d7a2e5e-bfda-4dbd-9a00-8e2af7451f51

**Froyok (Léna Piquet) — OMBRE Dev-Blog, fog blur / SSMS.** Понятное описание «fog-aware bloom» и его главного артефакта — ореолов на разрывах глубины.
- https://www.froyok.fr/blog/2024-11-ombre-dev-blog-2/

**OCASM/SSMS (Unity).** Открытая реализация той же идеи.
- https://github.com/OCASM/SSMS

**Dmitry Karpukhin — Screen Space Fog Scattering (UE), документация.** Коммерческий плагин, который заказчик считает лучше штатного FSSS. Только документация.
- https://dmkarpukhin.com/docs/ssfs/

**UE 5.8 Release Notes — Fog Screen Space Scattering (FSSS).**
- https://dev.epicgames.com/documentation/unreal-engine/unreal-engine-5-8-release-notes

---

## 5. Инфраструктура UE (понадобится на этапе B)

- Render Dependency Graph: https://dev.epicgames.com/documentation/en-us/unreal-engine/render-dependency-graph-in-unreal-engine
- Adding Global Shaders: https://dev.epicgames.com/documentation/unreal-engine/adding-global-shaders-to-unreal-engine
- Lumen Technical Details (surface cache, distance fields, HWRT, ограничения тонких стен): https://dev.epicgames.com/documentation/en-us/unreal-engine/lumen-technical-details-in-unreal-engine
- Wright и др. — «Lumen: Real-Time Global Illumination in Unreal Engine 5», SIGGRAPH 2022 (Advances) — в т.ч. как Lumen кормит volumetric fog. **[URL не проверен]**
- Kovalovs — «Volumetric Effects of The Last of Us Part II», SIGGRAPH 2020 — практика froxel-тумана в продакшене: джиттер, temporal, композит. **[URL не проверен]**
- Bauer — «Creating the Atmospheric World of Red Dead Redemption 2», SIGGRAPH 2019 — [доклад и официальный PPTX](https://www.advances.realtimerendering.com/s2019/index.htm). Проверено 2026-09-21: слайд 35 — фильтрованная cloud ESM; 66–70 — sky irradiance и reflections с scattering/transmittance. Применение к FogMS и границы — `FogMS_A1f_Research.md`.

### Дополнение заказчика: Nubis³ (2026-09-20)

**Andrew Schneider / Guerrilla — «Nubis³: Methods (and madness) to model and render immersive real-time voxel-based clouds», SIGGRAPH 2023.** Приоритетный production-референс для локального объёма с камерой внутри: моделирование 3D-плотности, compressed SDF для ускорения марша, детализация воксельных данных, ускорение light sampling, cloud-specific inner glow/dark edges.
- https://www.guerrilla-games.com/read/nubis-cubed
- Материалы курса: https://www.advances.realtimerendering.com/s2023/index.html
- PDF: https://d3d3g8mu99pzk9.cloudfront.net/AndrewSchneider/Nubis%20Cubed.pdf

**Проверено:** описание и релевантные разделы PDF, скачанного отдельно после отказа web-fetch. Освещение PDF 126–157 прочитано; формулы/схемы 129/142/150 проверены визуально. Поправки к присланному research и исходники возможностей UE — `FogMS_Nubis_Review.md`. Полное чтение всех 220 страниц не заявляется. Применимость к native UE froxel grid требует проверки; текущий A1b не является реализацией Nubis³.

---

## 6. Если времени мало — порядок чтения

1. Wronski 2014 (слайды) → 2. Frostbite 2016 course notes, главы про participating media и облака → 3. Billeter 2012.
Этого достаточно, чтобы понимать все три наших режима: самозатенение, октавы, пространственный перенос.

---

## 7. Освещение облаков: облик, фаза, стабильность (добавлено 2026-09-24)

Разбор по восьми пунктам (MS, фаза, тёмные края, ambient, тени, время, лампы, цена) и применимость к Box —
`FogMS_Cloud_Lighting_Review.md`. Здесь только ссылки, проверенные 24.09.

- **Schneider, Vos — HZD 2015 (PDF, прочитан):** https://d3d3g8mu99pzk9.cloudfront.net/AndrewSchneider/The-Real-time-Volumetric-Cloudscapes-of-Horizon-Zero-Dawn.pdf — Beer-Powder, HG, конус из 6 выборок, 1/16 пикселей за кадр.
- **Schneider — Nubis 2017 и Nubis, Evolved 2022 (страницы):** https://www.guerrilla-games.com/read/nubis-authoring-real-time-volumetric-cloudscapes-with-the-decima-engine , https://www.guerrilla-games.com/read/nubis-evolved . PDF больше 10 МБ, не прочитаны; модель 2022 года пересказана автором в Nubis³ (стр. PDF 29–39).
- **Hillaire — SIGGRAPH 2016, слайды (SlideShare, прочитаны):** https://www.slideshare.net/DICEStudio/physically-based-sky-atmosphere-and-cloud-rendering-in-frostbite . Заметки курса по новому адресу: https://media.contentapi.ea.com/content/dam/eacom/frostbite/files/s2016-pbs-frostbite-sky-clouds-new.pdf (больше 10 МБ, не прочитаны). Адрес из §2 24.09 не ответил, страница новости EA — 404.
- **Wrenninge — Art-Directable Multiple Volumetric Scattering, 2015 (PDF, прочитан):** https://history.siggraph.org/wp-content/uploads/2022/10/2015-Talks-Wrenninge_Art-Directable-Multiple-Volumetric-Scattering.pdf . Ссылка на Oz 2013 из §2 отдаёт 404; запись: https://www.researchgate.net/publication/262309690_Oz_the_great_and_volumetric **[URL не проверен]**.
- **Jendersie, d'Eon — An Approximate Mie Scattering Function for Fog and Cloud Rendering, SIGGRAPH 2023 Talks (PDF, прочитан):** https://research.nvidia.com/labs/rtr/approximate-mie/publications/approximate-mie.pdf — смесь HG и Draine, диаметр капли 5–50 мкм, без глории и радуги тумана.
- **Bouthors et al. — Interactive Multiple Anisotropic Scattering in Clouds, I3D 2008 (страница):** https://maverick.inria.fr/Publications/2008/BNMBC08/
- **Kallweit et al. — Deep Scattering, SIGGRAPH Asia 2017:** https://arxiv.org/abs/1709.05418
- **Kovalovs — Volumetric Effects of The Last of Us: Part Two, SIGGRAPH 2020 Talks (PDF, прочитан):** https://history.siggraph.org/wp-content/uploads/2022/08/2020-Talks-Kovalovs_Volumetric-Effects-of-The-Last-of-Us-Part-Two.pdf — свет ламп не накапливается во времени, накапливаются только их тени. Заменяет пометку **[URL не проверен]** в §5.
- **Bauer — RDR2 2019:** PPTX прочитан целиком по тексту слайдов и заметок (слайды 28–57: фаза, тени, фроксели, время, цена).
