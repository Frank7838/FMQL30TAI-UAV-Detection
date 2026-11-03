"""取得VisioDECT图像及配套TXT、XML标签。"""

from common import *
import argparse, re, struct, time, traceback, zlib

OUT = W / "visiodect_train_pilot_v1"
INDEX = W / "visiodect_index_v1/INDEX_ZIP64_RESOLVED.json"
RES = W / "visiodect_index_v1/SOURCE_ROLE_RESERVATION.json"
URL = "https://www.kaggle.com/api/v1/datasets/download/simeonajakwe/visiodect-for-scenario-based-multi-drone-detection?datasetVersionNumber=1"


# 确定训练来源组与带配对标签的抽样文件。
def freeze():
    index = read(INDEX)
    roles = read(RES)
    assert sha(INDEX) == roles["index_SHA256"]
    rows = {r["name"]: r for r in index["entries"]}
    selections = []
    for group in ["Anafi-Extended", "DJIFPV", "DJIPhantom"]:
        assert roles["group_roles"][group] == "TRAIN_CANDIDATE"
        base = "VisioDECT Dataset Upload/" + group + "/"
        candidates = []
        for name, row in rows.items():
            if not name.startswith(base + "images/Sunny/") or not name.lower().endswith(".jpg"):
                continue
            stem = Path(name).stem
            txt = base + "labels/sunny/txt/" + stem + ".txt"
            xml = base + "labels/sunny/voc/" + stem + ".xml"
            if txt in rows and xml in rows:
                candidates.append((int(re.search(r"\((\d+)\)$", stem).group(1)), name, txt, xml))
        candidates.sort()
        assert len(candidates) >= 12, (group, len(candidates))
        for position in [round(i * (len(candidates) - 1) / 11) for i in range(12)]:
            ordinal, *names = candidates[position]
            selections.append(
                dict(
                    group=group,
                    ordinal=ordinal,
                    eligible_pool=len(candidates),
                    members=[rows[n] for n in names],
                )
            )
    assert len(selections) == 36
    OUT.mkdir(exist_ok=False)
    plan = dict(
        at=stamp(),
        source_id=roles["source_id"],
        source_version=1,
        url=URL,
        archive_bytes=index["archive_bytes"],
        selection="12 equally spaced matched image/TXT/XML stems per whole TRAIN candidate model group, Sunny only; selected from metadata before media",
        role="TRAIN_CANDIDATE_NOT_ADMITTED",
        selections=selections,
        max_network_bytes=24000000,
        max_seconds=1200,
        max_uncompressed_member_bytes=4000000,
        frozen_files={
            str(p): sha(p)
            for p in [
                INDEX,
                RES,
                W / "DATA_ROLE_REGISTRY.json",
                W / "data_role_guard.py",
                Path(__file__),
            ]
        },
        model_forwards=0,
        training_updates=0,
        final_media=0,
        limits="No DEV/final member retrieval. No inference. No nearest-label substitution. Missing or uncertain labels never negatives. Group/site independence unresolved. Admission requires full-image visual and label QA.",
    )
    write_new(OUT / "PLAN.json", plan)
    print(
        "FROZEN",
        len(selections),
        "images",
        sum(len(s["members"]) for s in selections),
        "members",
        flush=True,
    )


# 按归档偏移取得图像、TXT与XML标签。
def acquire():
    import requests

    plan = read(OUT / "PLAN.json")
    for path, digest in plan["frozen_files"].items():
        assert sha(path) == digest, path
    write_new(
        OUT / "STARTED.json", dict(at=stamp(), pid=os.getpid(), plan_SHA256=sha(OUT / "PLAN.json"))
    )
    start = time.monotonic()
    used = 0
    records = []
    session = requests.Session()

    def fetch(lo, n):
        nonlocal used
        assert n > 0 and used + n <= plan["max_network_bytes"]
        assert time.monotonic() - start < plan["max_seconds"]
        with session.get(
            URL,
            headers={"Range": f"bytes={lo}-{lo+n-1}", "Accept-Encoding": "identity"},
            stream=True,
            timeout=(15, 45),
        ) as r:
            assert r.status_code == 206, ("refuse non-range response", r.status_code)
            assert (
                r.headers.get("Content-Range") == f'bytes {lo}-{lo+n-1}/{plan["archive_bytes"]}'
            ), r.headers.get("Content-Range")
            parts = []
            got = 0
            for b in r.iter_content(65536):
                got += len(b)
                used += len(b)
                assert (
                    got <= n
                    and used <= plan["max_network_bytes"]
                    and time.monotonic() - start < plan["max_seconds"]
                )
                parts.append(b)
            assert got == n
            return b"".join(parts)

    try:
        for idx, selection in enumerate(plan["selections"]):
            for member in selection["members"]:
                offset = member["local_header_offset"]
                h = struct.unpack("<4s5H3L2H", fetch(offset, 30))
                assert h[0] == b"PK\x03\x04" and not h[2] & 1 and h[3] == member["compression"]
                name_len, extra_len = h[9:11]
                assert member["size"] <= plan["max_uncompressed_member_bytes"]
                payload = fetch(offset + 30, name_len + extra_len + member["compressed_size"])
                canonical = payload[:name_len].decode("utf-8" if h[2] & 0x800 else "cp437")
                assert canonical == member["name"]
                compressed = payload[name_len + extra_len :]
                assert member["compression"] in (0, 8)
                raw = zlib.decompress(compressed, -15) if member["compression"] == 8 else compressed
                assert (
                    len(raw) == member["size"] and zlib.crc32(raw) & 0xFFFFFFFF == member["CRC32"]
                )
                target = (
                    OUT
                    / f'{idx:03d}_{selection["group"]}_{selection["ordinal"]}{Path(canonical).suffix.lower()}'
                )
                with target.open("xb") as f:
                    f.write(raw)
                records.append(
                    dict(
                        group=selection["group"],
                        ordinal=selection["ordinal"],
                        canonical_member=canonical,
                        local=str(target),
                        SHA256=sha(target),
                        bytes=len(raw),
                        CRC32=member["CRC32"],
                    )
                )
                update(
                    OUT / "PROGRESS.json",
                    dict(
                        at=stamp(),
                        members_complete=len(records),
                        network_bytes=used,
                        elapsed_s=round(time.monotonic() - start, 2),
                        model_forwards=0,
                    ),
                )
            print("IMAGE_TRIPLET_COMPLETE", idx + 1, "/36", flush=True)
        write_new(
            OUT / "ACQUISITION_RESULT.json",
            dict(
                at=stamp(),
                records=records,
                network_bytes=used,
                elapsed_s=round(time.monotonic() - start, 3),
                member_integrity="All selected members size/CRC32/SHA256 verified; not whole archive digest verification",
                model_forwards=0,
                training_updates=0,
                final_media=0,
            ),
        )
    except BaseException:
        write_new(
            OUT / "FAILURE.json",
            dict(
                at=stamp(),
                traceback=traceback.format_exc(),
                records=records,
                network_bytes=used,
                elapsed_s=round(time.monotonic() - start, 3),
            ),
        )
        raise


if __name__ == "__main__":
    p = argparse.ArgumentParser()
    p.add_argument("--acquire", action="store_true")
    a = p.parse_args()
    acquire() if a.acquire else freeze()
