-- Executed inside the caller's transaction, on every start (idempotent).
-- スクールアイドルコネクト motion-capture replays: the official archive details
-- carry archive_url only for some archives (the rest had video replays, or the
-- field was dropped once the official replay went away). Give every archive that
-- has a motion-capture replay the entry the community mirror keeps (localify's
-- archive-config.json: external_fix_link, else external_link): an als playlist
-- entry (.md, client >= 4.0.1) or an mrs index (.iarc). The URL keeps the official
-- host; OfflineHttpServer points archive/ URLs at its asset proxy when a remote
-- asset base is configured.
-- mrs replays of With×MEETS archives are not listed: the 5.0.1 client cannot
-- play mrs (it reads the .iarc index but never fetches the .ias segments), so
-- those archives play their video replay instead (009_mrs_video_replays.sql).
CREATE TEMP TABLE IF NOT EXISTS archive_replay_url (archives_id TEXT PRIMARY KEY, url TEXT NOT NULL);
DELETE FROM archive_replay_url;
INSERT INTO archive_replay_url(archives_id, url) VALUES
('fc5e44f5-93d8-447d-8d96-d411f38fedf0', 'https://assets.link-like-lovelive.app/archive/alst/20250924-fix/index.md'),
('a6fe0908-6dcc-4774-a6ff-70f39a6d7911', 'https://assets.link-like-lovelive.app/archive/alst/20250821202000-5d4a5c14-5e28-4f5c-acf4-8bcc16d81e3f/811c9dc5a3601.md'),
('097ebe03-20a1-4f66-af9c-191c8dd5d992', 'https://assets.link-like-lovelive.app/archive/alst/20250816-fix/index.md'),
('4bdab8b2-0894-464b-ba11-9beec6224da4', 'https://assets.link-like-lovelive.app/archive/alst/20250811205000-70028180-db5d-46aa-832b-d6e8930870c3/811c9dc5b9928.md'),
('fb796878-537c-4274-b080-95dcea7041e0', 'https://assets.link-like-lovelive.app/archive/alst/20250809135000-54f044c9-b365-4091-8ed9-ee910d639616/811c9dc5b18c5.md'),
('d8bf693b-a0c0-4160-9c1f-df8d14c9b009', 'https://assets.link-like-lovelive.app/archive/alst/20250807202000-447951e3-5605-431a-aa49-24a2abfc7563/811c9dc5519c4.md'),
('195bb9fe-dd95-493f-bed6-2a59f38476f5', 'https://assets.link-like-lovelive.app/archive/alst/20250804202000-66451bcb-a668-4610-8f0c-cc37d4316a77/811c9dc5d23e9.md'),
('cc8534a4-b26a-4bd6-b6ba-658eef924e04', 'https://assets.link-like-lovelive.app/archive/alst/20250802-fix/index.md'),
('25d1ee5f-b665-4a9c-83c2-84f0297b154b', 'https://assets.link-like-lovelive.app/archive/alst/20250731202000-ab8f2e59-f485-4947-80cb-b2bbc634f205/811c9dc5b61ac.md'),
('e142cdd0-9a01-4708-b4a3-6cdfcb451db0', 'https://assets.link-like-lovelive.app/archive/alst/20250728202000-fa8a2aa6-2fbf-4c61-9508-93ac1625657f/811c9dc56a09c.md'),
('d11fcef3-973c-4754-9393-1501ace2744d', 'https://assets.link-like-lovelive.app/archive/alst/20250726135000-20bc0bfd-12a4-4384-8c6c-09784756a4f4/811c9dc5c0e18.md'),
('9a68627f-9f17-4519-aea3-382c5084e9c1', 'https://assets.link-like-lovelive.app/archive/alst/20250724205000-0e4e9e9e-21b1-4b08-a902-7119d910f059/811c9dc59c503.md'),
('35429742-42dc-472d-a397-ea255f6738ef', 'https://assets.link-like-lovelive.app/archive/alst/20250721202000-c2d0a53b-3290-47ca-ad04-66c4bed3ff4f/811c9dc568766.md'),
('af2cc5a9-24af-4f81-921c-ef13a0110217', 'https://assets.link-like-lovelive.app/archive/alst/20250719135000-b3a603da-98e3-4eb8-90e7-3774c0ddc0ff/811c9dc5c671a.md'),
('4ccfce94-d745-4859-ae13-33bd8a27e128', 'https://assets.link-like-lovelive.app/archive/alst/20250717202000-ad0fc0f2-559b-493b-a2d1-1f76aab05108/811c9dc5dfc47.md'),
('bdb11592-9b22-4fe1-80ec-f3b59197cc05', 'https://assets.link-like-lovelive.app/archive/alst/20250714202000-5a813934-3ae8-4f79-966d-7cd5b67a7789/811c9dc5e7f31.md'),
('ef206260-880f-4c94-a369-5ae7aa0f26e5', 'https://assets.link-like-lovelive.app/archive/alst/20250712135000-d7479474-b9a1-4648-a0c7-5232381ae084/811c9dc5a47e0.md'),
('85db5cab-660b-42bc-ba3b-5a0e12d1ef08', 'https://assets.link-like-lovelive.app/archive/alst/20250707202000-95ed86e5-6be9-4d67-ab0f-933cf08caecb/811c9dc5d8f94.md'),
('e2ddbaa3-a715-4dbb-b8df-b585be1a1ed8', 'https://assets.link-like-lovelive.app/archive/alst/20250703202000-61bffb9e-c19b-4636-b1a0-2e9a7ef8e4dc/811c9dc570aa0.md'),
('5572e4cb-7218-40c7-a9ff-5e654a367856', 'https://assets.link-like-lovelive.app/archive/alst/20250629195001-c3f0d1d9-d61e-411e-b225-44f8bfd8fc80/811c9dc52a290.md'),
('6a243c3d-8e74-49ec-b13a-9df0620a785a', 'https://assets.link-like-lovelive.app/archive/alst/20250628135000-ca55a95d-602a-43d4-929e-0a451aae9e77/811c9dc5c8a9b.md'),
('444ccd2a-dfbe-4edc-912c-1e75505e72a7', 'https://assets.link-like-lovelive.app/archive/alst/20250626202000-ac26f837-840f-415f-9020-4dd17363e8c3/811c9dc5a9e1b.md'),
('afc1e7ab-82b7-432b-8a53-70c0fc050222', 'https://assets.link-like-lovelive.app/archive/alst/20250623202000-7688bc1b-9dad-4fc0-8a7e-d13fe285386f/811c9dc5760f2.md'),
('05c97aa3-5dc3-4ea0-a291-d6d9f0be75e5', 'https://assets.link-like-lovelive.app/archive/alst/20250621-fix/index.md'),
('f64a1bb9-9600-4b3b-bc8b-5d4b4e777bc2', 'https://assets.link-like-lovelive.app/archive/alst/20250619202000-30a39a79-3929-401a-a687-44cede26c48b/811c9dc5a29c0.md'),
('65569491-60ab-4581-a986-f771668ee5aa', 'https://assets.link-like-lovelive.app/archive/alst/20250616205000-3d6ba98c-abbd-4055-9aea-bff37dc3e9fd/811c9dc594063.md'),
('061419d3-e2e1-4da5-a1d1-550375598f4f', 'https://assets.link-like-lovelive.app/archive/alst/20250614135000-6d1a85d0-488c-49a5-9a7a-48a667a9b0e0/811c9dc59920c.md'),
('7bee4cd9-64e5-4f3a-a1f9-3cae5bd82820', 'https://assets.link-like-lovelive.app/archive/alst/20250612-fix/index.md'),
('989701d1-50a7-4f36-b744-e5b3f5670bcc', 'https://assets.link-like-lovelive.app/archive/alst/20250604202001-ef515232-ba04-48d8-8bbc-cb80b2902bea/811c9dc5296c2.md'),
('facbda51-bb9d-4267-b4ab-5ef3c878baec', 'https://assets.link-like-lovelive.app/archive/alst/20250529202000-5e0eb4db-e6fc-47a1-bcdc-997919b76a8c/811c9dc5a1081.md'),
('8bd2ec46-5b88-4f15-b670-92e06d526db2', 'https://assets.link-like-lovelive.app/archive/mrs/20250430195000-818dca6c06e9213200aba4735f507987/archive_1385606917_20250430195000-905728.iarc'),
('3deab2d6-09b7-4f6d-9677-74240cda09db', 'https://assets.link-like-lovelive.app/archive/mrs/20250330195000-ee0fa51489e2731abe2420ffebfae0b3/archive_131928012_20250330195000-782864.iarc'),
('77cd3cdc-ea3d-49f4-9e1c-8bfcba2652f6', 'https://assets.link-like-lovelive.app/archive/mrs/yyflll/20250126204501-698173282a99e322a226c94082687fff/archive_63455343_20250126204501-360196.iarc'),
('ec0c0a07-c6f4-4999-9bf8-5051efa551c1', 'https://assets.link-like-lovelive.app/archive/mrs/yyflll/20250126195000-de8c751a5bf8c6587081d573c745e210/archive_1632942133_20250126195000-671916.iarc'),
('8441b7d0-4091-43d3-816a-18d9860c7027', 'https://assets.link-like-lovelive.app/archive/mrs/yyflll/20241028204500-330009ec34fd3570a58ecee1b94a4c59/archive_1806429982_20241028204500-795771.iarc'),
('9e5ac8a8-54c3-4586-8a55-eea0e17502d5', 'https://assets.link-like-lovelive.app/archive/mrs/yyflll/20241028195001-74c04eda740505f41fda9dfd08cc634e/archive_327334386_20241028195001-033296.iarc'),
('b333dc32-76bf-41cc-bc6c-11b39a5eac6f', 'https://assets.link-like-lovelive.app/archive/mrs/case/20240731195057-2940812e126b393586a8f71b2f2c9e62/archive_394124401_20240731195057-039371.iarc'),
('8e6f3c5f-7e80-4dbf-be86-1ac797423933', 'https://assets.link-like-lovelive.app/archive/mrs/case/20240429200005-38e1af730cf4380608e7d21daf2d6c98/archive_2007818627_20240429200005-433786.iarc'),
('575124a7-b8f4-4978-b70a-5f649cecaf78', 'https://assets.link-like-lovelive.app/archive/mrs/case/20240328195024-98e1e70f435c985267da1e23aae04b14/archive_946096826_20240328195024-067892.iarc'),
('e75a05f8-0feb-4a47-a6b7-b76cc42f52a8', 'https://assets.link-like-lovelive.app/archive/mrs/case/20240229201005-7fd631235555e0b9bd840635dd052eb9/archive_471087133_20240229201005-221958.iarc'),
('3f361d68-2491-4596-91bc-4d5e45c89b2a', 'https://assets.link-like-lovelive.app/archive/mrs/case/20240131195002-a62f3795c1a4a1c14573583c32c27b3d/archive_2141747917_20240131195002-154690.iarc'),
('15c00170-c2da-4d74-b387-4692b86df207', 'https://assets.link-like-lovelive.app/archive/mrs/case/20231228195000-558ce83903fa00c294037fd2c25eb37f/archive_1071901217_20231228195000-337683.iarc'),
('a50f4a67-0222-46ab-8ac9-10d1ae47353f', 'https://assets.link-like-lovelive.app/archive/mrs/case/20231129195000-68fa08a506832672f6aebd963df8021f/archive_369019502_20231129195000-240948.iarc'),
('f9cb4aaf-5c98-4054-a2cf-9de103303f25', 'https://assets.link-like-lovelive.app/archive/mrs/case/20231030202002-7cd7e886aae154056a1bcd5d91356edc/archive_1194174351_20231030202002-162100.iarc'),
('025816e3-0c7b-4fa3-b552-423a770730a7', 'https://assets.link-like-lovelive.app/archive/mrs/case/20231030185004-0c8d06ed4d5a18cb6d88ff93f7821efd/archive_1170966227_20231030185004-824879.iarc');
UPDATE archive_detail
SET response_json = json_set(response_json, '$.archive_url', (SELECT url FROM archive_replay_url AS r WHERE r.archives_id = archive_detail.archives_id)),
    archive_url = (SELECT url FROM archive_replay_url AS r WHERE r.archives_id = archive_detail.archives_id)
WHERE archives_id IN (SELECT archives_id FROM archive_replay_url)
  AND COALESCE(json_extract(response_json, '$.archive_url'), '') <> (SELECT url FROM archive_replay_url AS r WHERE r.archives_id = archive_detail.archives_id);
-- With×MEETS: the client plays the video replay whenever the detail has a
-- video_url and the motion-capture replay only when it has none (the official
-- 2025 archives that kept only archive_url). No video replay survived, so drop
-- video_url from With×MEETS archives that have a motion-capture replay. Fes×LIVE
-- details (they carry selectable_camera_types) open the 3D replay either way.
UPDATE archive_detail
SET response_json = json_remove(response_json, '$.video_url')
WHERE archives_id IN (SELECT archives_id FROM archive_replay_url UNION
                      SELECT archives_id FROM archive_detail WHERE COALESCE(json_extract(response_json, '$.archive_url'), '') <> '')
  AND json_extract(response_json, '$.selectable_camera_types') IS NULL
  AND json_extract(response_json, '$.video_url') IS NOT NULL;
DROP TABLE archive_replay_url;
