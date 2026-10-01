# Face Recognition Evaluation

This folder contains the two face-recognition experiments that informed the backend's verification threshold. The project uses the pretrained **InsightFace `buffalo_l`** model; the work here was to evaluate its embeddings, compare images of the same and different people, and decide how to use the model in our kiosk. We did **not** train a new neural network.

Both graphs show **embedding-distance distributions**, not training curves or a general accuracy score. Their results describe the image pairs evaluated during development, rather than guaranteeing the same outcome for every new person or ESP32 camera capture.

## Dataset

The offline experiments used a **249-subject selection from the [CMU Multi-PIE Face Database](https://www.cs.cmu.edu/afs/cs/project/PIE/MultiPie/Multi-Pie/Home.html)**. Multi-PIE contains images of 337 people photographed across multiple recording sessions, camera viewpoints, facial expressions, and lighting conditions. These controlled variations made it useful for testing how the face embeddings change when a person is photographed from different angles or under different illumination.

The filenames follow the Multi-PIE convention. For example, `008_01_01_050_06_crop_128.png` identifies subject `008`, session `01`, recording `01`, camera `050`, and image/illumination code `06`. The `crop_128` suffix indicates that the copy used in our work was cropped to 128 × 128 pixels; it is not part of the original Multi-PIE naming convention. Camera IDs describe capture viewpoints, rather than measurements of how far the subject turned their head. The exact cropping procedure is not documented here.

Only the selected images were used for our experiments; we did **not** train or fine-tune InsightFace on Multi-PIE. The source images are not distributed in this repository. To obtain the dataset, consult CMU's official [Multi-PIE distribution page](https://www.cs.cmu.edu/afs/cs/project/PIE/MultiPie/Multi-Pie/Home.html) and its applicable terms.

**Dataset reference:** Ralph Gross, Iain Matthews, Jeffrey F. Cohn, Takeo Kanade, and Simon Baker, *Multi-PIE*, Proceedings of the 8th IEEE International Conference on Automatic Face & Gesture Recognition (FG 2008). [Publication](https://www.microsoft.com/en-us/research/publication/multi-pie/).

## Files

- [`experiment_1_controlled_face_distances.png`](experiment_1_controlled_face_distances.png) — comparisons using five selected views under one lighting condition.
- [`experiment_2_varied_lighting_face_distances.png`](experiment_2_varied_lighting_face_distances.png) — comparisons under more varied image conditions.

## How face verification fits into the kiosk

The backend already knows which user is attempting authentication because the first factor is RFID or employee ID and PIN. If the backend selects `FACE` as the randomized second factor, the ESP32 uploads a camera image, and the backend compares it against **that user's** enrollment representation. We verify an expected identity rather than trying to identify an unknown person from the whole database.

```text
RFID or ID + PIN -> expected user -> FACE selected
                                        |
                                   ESP32 image
                                        |
                             OpenCV + InsightFace
                                        |
                                face embedding
                                        |
                           compare enrolled vector
                                        |
                                   accept / reject
```

### Embeddings and Euclidean distance

InsightFace converts a detected face into a **512-dimensional vector**, called an embedding. We use normalized embeddings so comparisons are made on a consistent scale. Two images of the same person should generally produce more similar embeddings, even though the photographs themselves have different pixels.

We compare two vectors with **Euclidean distance**:

```text
distance = sqrt(sum((embedding_A[i] - embedding_B[i]) ** 2))
```

A smaller distance means the model produced more similar facial representations; a larger distance means it produced more different representations. That gives us two comparison groups:

- **Genuine pairs:** two images of the same person. We expect smaller distances.
- **Impostor pairs:** images of different people. We expect larger distances.

The important part of the graphs is the distance between the **largest observed genuine value** and the **smallest observed impostor value**. A gap between those groups gives us a region in which to place a decision threshold for the evaluated data.

## Experiment 1: controlled conditions

![Experiment 1 — controlled face distances](experiment_1_controlled_face_distances.png)

### Objective and image selection

The first experiment provided a baseline with **249 subjects**. We used five view codes per subject (`051`, `140`, `050`, `130`, and `041`) while holding the recorded lighting code at `06`. This introduced multiple head views without adding the harder lighting variation tested in the second experiment.

The codes correspond to Multi-PIE camera IDs and illumination/image numbers. They identify the selected captures without assuming a precise head-turn angle for each camera. The mapping from the original dataset files to the cropped images and any exclusions during preprocessing would require the original experiment script to reproduce exactly.

### Comparison procedure

Each selected image was processed through InsightFace to obtain a normalized embedding. Euclidean distances were collected for pairs belonging to the same subject and pairs belonging to different subjects.

With five selected images per subject, there are ten distinct same-person pairs per identity. Comparing every image combination across each distinct pair of subjects produces the reported counts:

```text
Genuine:  249 × 10 = 2,490 comparisons
Impostor: (249 × 248 / 2) × 5 × 5 = 771,900 comparisons
```

These calculations explain the reported totals. The original execution script would still be needed to reproduce the exact filtering, file selection, and treatment of any failed face detections.

### Results

| Measurement | Genuine pairs | Impostor pairs |
| --- | ---: | ---: |
| Comparisons | 2,490 | 771,900 |
| Minimum distance | 0.2735 | 1.0593 |
| Average distance | 0.4862 | 1.3947 |
| Maximum distance | 0.8133 | 1.5783 |

The largest observed genuine distance was **0.8133**; the smallest observed impostor distance was **1.0593**. The gap between those values was approximately **0.246**.

### Reading the first graph

The genuine distribution occupies the lower-distance region, and the impostor distribution occupies the higher-distance region. **No overlap was recorded** between the two ranges in this experiment. The space between them means a threshold inside that region would separate all the pairs evaluated here.

The averages are useful for understanding typical comparisons, but the extremes matter more when choosing a threshold. A threshold based only on the genuine average (**0.4862**) would reject many genuine pairs recorded above it. Likewise, the closest observed impostor (**1.0593**) matters more for a decision boundary than the much higher impostor average (**1.3947**).

## Experiment 2: varied lighting and capture conditions

![Experiment 2 — varied lighting face distances](experiment_2_varied_lighting_face_distances.png)

### Objective

The second experiment tested whether the separation from the controlled experiment remained when genuine images were less consistent, especially under increased lighting or capture variation. A kiosk user will not reproduce precisely the same conditions at enrollment and authentication, so this is a more difficult comparison.

### Procedure and recorded results

The experiment was recorded as the **varied-lighting study**, but the exact additional lighting codes, full image-selection rules, and complete run statistics were not retained in the available project notes. Rather than guess those details, this README reports the measurements that were preserved:

| Measurement | Recorded value |
| --- | ---: |
| Average genuine distance | 0.589 |
| Maximum genuine distance | 0.906 |
| Minimum impostor distance | approximately 1.059 |
| Gap between those extremes | approximately 0.153 |

The missing comparison count and other summary values have not been filled in with estimates.

### Reading the second graph

The same-person comparisons moved toward **larger distances** as capture conditions became less consistent. The average genuine distance increased from **0.486** to **0.589**, and the maximum increased from **0.813** to **0.906**.

The closest reported impostor remained near **1.059**. The observed separation therefore narrowed from roughly **0.246** in Experiment 1 to **0.153** in Experiment 2. The recorded ranges still did not overlap, but the smaller gap shows why selecting a threshold from only the easiest captures would be misleading.

## Comparison and threshold selection

| Result | Experiment 1 | Experiment 2 |
| --- | ---: | ---: |
| Average genuine distance | 0.486 | 0.589 |
| Maximum genuine distance | 0.813 | 0.906 |
| Minimum impostor distance | 1.059 | ~1.059 |
| Observed gap | ~0.246 | ~0.153 |

### Candidate threshold on the graphs: 0.93

The development graphs mark a candidate threshold near **0.93**. This sits above the genuine distances and below the impostor distances recorded in both experiments. It was a useful boundary for interpreting the development data, not a claim that the value works for every new face or environment.

### Deployed backend threshold: 0.95

The backend uses a Euclidean-distance threshold of **0.95**:

```text
distance <= 0.95  -> face match accepted
distance >  0.95  -> face match rejected
```

This setting is slightly more tolerant than the value marked on the graphs. In Experiment 2, it is **0.044** above the largest observed genuine distance (**0.906**) and **0.109** below the smallest reported impostor distance (**1.059**).

A more permissive threshold may accept more legitimate capture variation, but it also moves closer to different-person comparisons. **0.95 is the project's chosen setting, informed by these experiments—not a universal threshold for InsightFace.**

## How the deployed enrollment differs from the experiments

The offline graphs compare embeddings from **individual images**. The deployed system instead builds one enrollment representation from **five prompted captures** and compares a later authentication image against that stored representation. The graphs should not be treated as a direct measurement of the five-image system's accuracy.

### Five-image enrollment

The kiosk requests these head positions:

1. `LOOK_STRAIGHT`
2. `TURN_SLIGHTLY_LEFT`
3. `TURN_MORE_LEFT`
4. `TURN_SLIGHTLY_RIGHT`
5. `TURN_MORE_RIGHT`

For every accepted capture, the backend extracts a normalized 512-dimensional embedding. It averages the five embeddings and then **normalizes the average** before saving it as the user's enrollment representation in PostgreSQL.

The idea is to include more than one head orientation in the stored representation. We did **not** run a separate controlled experiment comparing five-image enrollment with single-image enrollment, so we do not claim a measured improvement from this design.

### Authentication with the ESP32 camera

At authentication, the ESP32 sends a new capture to the backend. OpenCV decodes the image, InsightFace extracts its embedding, and the backend compares it with the expected user's stored representation using the **0.95** threshold.

```text
Five enrollment captures              One authentication capture
            |                                     |
      Five embeddings                        One embedding
            |                                     |
    Average + normalize                            |
            |                                     |
    Stored user embedding --------> Euclidean distance
                                                   |
                                           Compare to 0.95
                                                   |
                                             Accept / reject
```

The physical ESP32 camera introduces differences in framing, cropping, exposure, compression, and image quality that the offline dataset cannot fully reproduce. During the reported integration tests, the camera issue was resolved and the kiosk could communicate with the backend; the offline plots are **not** quantitative measurements of recognition performance on ESP32 captures.

## What the studies show

The controlled experiment recorded a **0.246** gap between its most difficult genuine pair and closest impostor pair. The varied-condition experiment raised genuine distances and reduced that gap to **0.153**, without observed overlap in the preserved results. Those observations provided a practical basis for investigating the backend's threshold.

These are results for the **evaluated image pairs**. They do not establish how the system will behave for every untested person, lighting condition, or camera. In particular, the project implements **face verification**, not dedicated liveness detection: matching an embedding does not independently establish that the camera is viewing a live person rather than a photograph or screen.

## Reproducing the experiments

The figures, recorded numerical results, and dataset source are included here. Exact reproduction still requires the original image-cropping and filtering procedure, the complete Experiment 2 image-selection rules, the handling of any unsuccessful face detections, and the original experiment script or equivalent execution instructions. Access to the underlying images is subject to the dataset provider's terms.

If those materials are recovered, they can be added without changing or inventing the current results. The most useful follow-up evaluation would use consented captures from the actual ESP32 camera and record the resulting same-person and different-person distances through the deployed enrollment/authentication pipeline.
