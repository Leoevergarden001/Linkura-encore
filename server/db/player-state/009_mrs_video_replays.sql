-- Executed inside the caller's transaction, on every start (idempotent).
-- With×MEETS archives whose motion-capture replay is mrs: the 5.0.1 client
-- cannot play mrs (it reads the .iarc index but never fetches the .ias
-- segments) and waited forever. Every one of them has the official video
-- replay (archive.org preservation items; the asset base serves static
-- playlists that point at them), so give the detail its video_url back and
-- drop the mrs archive_url that earlier versions of 008 set. With×MEETS plays
-- video whenever video_url is present. Fes×LIVE archives always open the 3D
-- replay, so their mrs entries stay as they are.
CREATE TEMP TABLE IF NOT EXISTS mrs_video_replay (archives_id TEXT PRIMARY KEY, video_url TEXT NOT NULL);
DELETE FROM mrs_video_replay;
INSERT INTO mrs_video_replay(archives_id, video_url) VALUES
('01affe06-98e4-4c96-bb39-e70a5bada2bc', 'https://assets.link-like-lovelive.app/archive/hls/20240502/index.m3u8'),
('05c3c6e8-f36c-4d41-bed1-529f8974fbdf', 'https://assets.link-like-lovelive.app/archive/hls/20240330/index.m3u8'),
('086170f6-3000-431b-bd19-4c5f980e5091', 'https://assets.link-like-lovelive.app/archive/hls/20231116/index.m3u8'),
('08db3ced-a8d9-4cb1-8c36-a11e8a8820d0', 'https://assets.link-like-lovelive.app/archive/hls/20241123/index.m3u8'),
('0a59e87e-1c83-4787-a87a-04e5a4460ca8', 'https://assets.link-like-lovelive.app/archive/hls/20250417/index.m3u8'),
('0bee2080-c954-4894-9cf1-73894b212481', 'https://assets.link-like-lovelive.app/archive/hls/20241102-2/index.m3u8'),
('18bfe3ee-8e80-48b8-9d37-934815b9e861', 'https://assets.link-like-lovelive.app/archive/hls/20240302/index.m3u8'),
('1cf1fae1-0290-4ba2-8ccb-778fa3b3e66b', 'https://assets.link-like-lovelive.app/archive/hls/20250210/index.m3u8'),
('1f273f18-c1b7-43df-91a0-45b6ee7059f1', 'https://assets.link-like-lovelive.app/archive/hls/20250220/index.m3u8'),
('234c47cc-a734-4ce3-9ed9-d819cd6d314d', 'https://assets.link-like-lovelive.app/archive/hls/20250412/index.m3u8'),
('24077bfe-5133-490c-9ac0-290e0fe065b5', 'https://assets.link-like-lovelive.app/archive/hls/20250421/index.m3u8'),
('2bf38570-ed47-4a38-9661-719912478a7b', 'https://assets.link-like-lovelive.app/archive/hls/20250129/index.m3u8'),
('2d2a274b-679f-474d-8cf6-98ce890998e6', 'https://assets.link-like-lovelive.app/archive/hls/20250403/index.m3u8'),
('331c599e-fe31-488c-bc01-f4f027212e9b', 'https://assets.link-like-lovelive.app/archive/hls/20250320/index.m3u8'),
('362ffb69-48d5-4080-99db-68916898f956', 'https://assets.link-like-lovelive.app/archive/hls/20250423/index.m3u8'),
('37d67b8d-54d3-4c5c-93c5-afca2e5f6492', 'https://assets.link-like-lovelive.app/archive/hls/20250324/index.m3u8'),
('3e4333f4-6c84-47aa-b779-b4f985cb2f92', 'https://assets.link-like-lovelive.app/archive/hls/20241216/index.m3u8'),
('46b0beff-25a3-4038-9ee8-807b2138d214', 'https://assets.link-like-lovelive.app/archive/hls/20250519-2nd/index.m3u8'),
('4711e1f9-2841-46f7-93c9-d5ea6196d754', 'https://assets.link-like-lovelive.app/archive/hls/20250222/index.m3u8'),
('4a83443a-6788-4b48-b75d-2b342867f1dd', 'https://assets.link-like-lovelive.app/archive/hls/20250322/index.m3u8'),
('4d2c10bb-aad3-4052-988b-614e6a636e96', 'https://assets.link-like-lovelive.app/archive/hls/20250419/index.m3u8'),
('4dd2228a-37ba-4ff7-bfff-6a3027605979', 'https://assets.link-like-lovelive.app/archive/hls/20250317/index.m3u8'),
('4f88658b-6a52-4939-bdf6-953fe314741b', 'https://assets.link-like-lovelive.app/archive/hls/20250303/index.m3u8'),
('58b3fea3-b27d-4bd6-9a65-b4b21b2dcd41', 'https://assets.link-like-lovelive.app/archive/hls/20250405/index.m3u8'),
('59025f5b-4904-4143-acee-ac93ceef0859', 'https://assets.link-like-lovelive.app/archive/hls/20241212/index.m3u8'),
('59ad56de-0ce2-4817-b44f-a0d2926fd0d6', 'https://assets.link-like-lovelive.app/archive/hls/20241230/index.m3u8'),
('6b06eaa8-42d7-4d3a-82dc-09ee37b31a3c', 'https://assets.link-like-lovelive.app/archive/hls/20241128/index.m3u8'),
('6d15ef02-614b-4d53-a77d-4229d85bd14c', 'https://assets.link-like-lovelive.app/archive/hls/20241130/index.m3u8'),
('6e90e113-ec0c-4fe8-b51f-754eeb3326c2', 'https://assets.link-like-lovelive.app/archive/hls/20231231/index.m3u8'),
('6eb69099-3b13-4f55-b776-27a090d18f1b', 'https://assets.link-like-lovelive.app/archive/hls/20250505/index.m3u8'),
('6fcd8351-6f90-4604-8082-aec0e65d374f', 'https://assets.link-like-lovelive.app/archive/hls/20241209/index.m3u8'),
('7351931a-0f6c-4790-bcc4-255510cc4c33', 'https://assets.link-like-lovelive.app/archive/hls/20250113/index.m3u8'),
('7799a8a1-a29f-40ed-be10-0b15fda9ca8c', 'https://assets.link-like-lovelive.app/archive/hls/20250124/index.m3u8'),
('7d2992e8-f5d0-418e-b9f1-32d591387516', 'https://assets.link-like-lovelive.app/archive/hls/20250215/index.m3u8'),
('7d74201b-d247-4818-91b8-020583f70116', 'https://assets.link-like-lovelive.app/archive/hls/20250313/index.m3u8'),
('7ec98633-6919-4789-80af-271b1f136e8b', 'https://assets.link-like-lovelive.app/archive/hls/20250206/index.m3u8'),
('80cb2019-62ef-4943-881a-5526eabed0ca', 'https://assets.link-like-lovelive.app/archive/hls/20250512/index.m3u8'),
('86cc9d11-4a37-41e7-b7eb-29d8aa7fb700', 'https://assets.link-like-lovelive.app/archive/hls/20241207/index.m3u8'),
('91c340d5-f3dc-4a7a-8bfa-1069909567a2', 'https://assets.link-like-lovelive.app/archive/hls/20241104/index.m3u8'),
('9b794337-b0d8-44eb-91fd-4c8487199b85', 'https://assets.link-like-lovelive.app/archive/hls/20250517/index.m3u8'),
('a0a678e2-6ea9-4edf-b7d2-5038ddf9e2b7', 'https://assets.link-like-lovelive.app/archive/hls/20241214/index.m3u8'),
('a40783d3-fe06-4807-a690-ffa7da0c3d1b', 'https://assets.link-like-lovelive.app/archive/hls/20250208/index.m3u8'),
('a64aff53-4329-43bd-ba5b-dfcd9180b6dc', 'https://assets.link-like-lovelive.app/archive/hls/20250308/index.m3u8'),
('aded6666-b8ef-4555-af05-ad4fdf4bffb1', 'https://assets.link-like-lovelive.app/archive/hls/20250310/index.m3u8'),
('b1e2d09b-3a57-44d9-951f-43fdfa08a0eb', 'https://assets.link-like-lovelive.app/archive/hls/20250510/index.m3u8'),
('b9b457f6-8e6e-4f34-ad8c-7d5e4882618f', 'https://assets.link-like-lovelive.app/archive/hls/20241121/index.m3u8'),
('bacf8358-cc14-4cd6-9b9d-ba4428883732', 'https://assets.link-like-lovelive.app/archive/hls/20250228/index.m3u8'),
('bceaddcb-addc-4042-ba65-5bee4b0d515a', 'https://assets.link-like-lovelive.app/archive/hls/20250501/index.m3u8'),
('c3c5f998-8997-4973-a44b-92f84224b4d8', 'https://assets.link-like-lovelive.app/archive/hls/20250118/index.m3u8'),
('c566a88f-17eb-4b91-8c31-db8cddc0c3e1', 'https://assets.link-like-lovelive.app/archive/hls/20231130/index.m3u8'),
('ca2daa8a-1a8b-449d-be5a-42fb1bd93487', 'https://assets.link-like-lovelive.app/archive/hls/20250515/index.m3u8'),
('d3200340-d9af-451e-b3ba-3167ff90f606', 'https://assets.link-like-lovelive.app/archive/hls/20250503/index.m3u8'),
('d33b01c9-b832-4351-b717-4b1297e6aada', 'https://assets.link-like-lovelive.app/archive/hls/20250120/index.m3u8'),
('d3e7dd5d-e70f-40cd-9d72-79f8e513daca', 'https://assets.link-like-lovelive.app/archive/hls/20250116/index.m3u8'),
('de74f414-b49b-4ba7-a0f9-26cb9e162158', 'https://assets.link-like-lovelive.app/archive/hls/20241228/index.m3u8'),
('dfceeca7-b7b7-4e0f-b2f2-d7d566c617d2', 'https://assets.link-like-lovelive.app/archive/hls/20250407/index.m3u8'),
('e5762784-72f8-4337-9d31-57aa0f015ccf', 'https://assets.link-like-lovelive.app/archive/hls/20241114/index.m3u8'),
('e5f29c79-3c84-4897-b8e3-9b961bd26e74', 'https://assets.link-like-lovelive.app/archive/hls/20231102/index.m3u8'),
('e6137bb3-be74-493e-980d-3d67a67cdd25', 'https://assets.link-like-lovelive.app/archive/hls/20240427/index.m3u8'),
('e77f7985-6a67-4a59-a689-d4870c96cf03', 'https://assets.link-like-lovelive.app/archive/hls/20241107/index.m3u8'),
('f3af0834-14e7-4a18-98ca-d01fdd6c8466', 'https://assets.link-like-lovelive.app/archive/hls/20241125/index.m3u8');
UPDATE archive_detail
SET response_json = json_set(json_remove(response_json, '$.archive_url'), '$.video_url',
                             (SELECT video_url FROM mrs_video_replay AS m WHERE m.archives_id = archive_detail.archives_id)),
    archive_url = ''
WHERE archives_id IN (SELECT archives_id FROM mrs_video_replay)
  AND (COALESCE(json_extract(response_json, '$.video_url'), '') <>
       (SELECT video_url FROM mrs_video_replay AS m WHERE m.archives_id = archive_detail.archives_id)
       OR json_extract(response_json, '$.archive_url') IS NOT NULL
       OR archive_url <> '');
DROP TABLE mrs_video_replay;
