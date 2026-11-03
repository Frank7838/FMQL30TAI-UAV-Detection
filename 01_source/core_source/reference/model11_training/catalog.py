"""定义各数据来源与本地目录。"""
from common import *
import subprocess


def main():
    names = subprocess.run(
        [
            "rg",
            "--files",
            "--no-ignore",
            "pc_runs",
            "weights",
            "-g",
            "*.pt",
            "-g",
            "!**/data/**",
            "-g",
            "!**/build/**",
        ],
        cwd=R,
        capture_output=True,
        text=True,
        check=True,
    ).stdout.splitlines()
    paths = sorted(set(str((R / p).resolve()) for p in names) | {str((R / "yolo11n.pt").resolve())})
    rows = []
    for value in paths:
        p = Path(value)
        role = "historical_or_research_checkpoint_not_current_product"
        decision = None
        if p == (R / "yolo11n.pt"):
            role = "generic_pretrained_rebuild_initializer"
        elif p == (R / "pc_runs/m370_day_candidate/weights/best.pt"):
            role = "FORMAL_RGB_M370_PRESERVE"
        elif W in p.parents:
            role = "new_rebuild_candidate_PC_only"
        elif "IR" in p.as_posix() or "ir_" in p.as_posix().lower():
            role = "IR_artifact_preserve_not_RGB_initializer"
        for parent in p.parents:
            if parent == R:
                break
            q = parent / "ROUND_CLOSED.json"
            if q.exists():
                d = read(q)
                decision = dict(
                    path=str(q),
                    PC_pass=d.get("PC_pass"),
                    product_promoted=d.get("product_promoted"),
                )
                break
        rows.append(
            dict(
                path=str(p),
                bytes=p.stat().st_size,
                SHA256=sha(p),
                role=role,
                decision_evidence=decision,
            )
        )
    update(
        W / "MODEL_CATALOG.json",
        dict(
            at=stamp(),
            records=rows,
            includes_git_ignored_checkpoints=True,
            policy="Index only,preserve sourcepaths; unknown historical roles not declared obsolete",
        ),
    )
    text = [
        "# 模型文件索引",
        "",
        f"包含Git忽略文件，共{len(rows)}个权重；无搬移或删除。正式RGB仅M370，重建初始权重为yolo11n.pt。",
        "",
        "|角色|路径|MiB|SHA256前12位|",
        "|---|---|---:|---|",
    ]
    for r in sorted(rows, key=lambda r: (r["role"], r["path"])):
        text.append(
            f"|{r['role']}|[{Path(r['path']).name}]({Path(r['path']).as_posix()})|{r['bytes']/1048576:.2f}|{r['SHA256'][:12]}|"
        )
    (W / "MODEL_CATALOG.md").write_text("\n".join(text), encoding="utf-8")
    print("MODEL_CATALOG_COMPLETE", len(rows))


if __name__ == "__main__":
    main()
