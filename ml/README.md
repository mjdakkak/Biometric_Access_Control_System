```md
# Face Recognition Evaluation

This folder documents the evaluation of the face-recognition component used by the biometric access control system.

Rather than training a new neural network, the project uses a pretrained InsightFace model to generate facial embeddings. The experiments in this folder were designed to answer a practical engineering question:

> Can embeddings produced for the same person be reliably separated from embeddings produced for different people, and what Euclidean-distance threshold is appropriate for this system?

The experiments compare **genuine** and **impostor** face distances under increasingly difficult image conditions.

The results were then used to guide the verification threshold implemented in the backend.

---

## Files

```text
ml/
├── README.md
├── experiment_1_controlled_face_distances.png
└── experiment_2_varied_lighting_face_distances.png
```

### `experiment_1_controlled_face_distances.png`

Evaluation under relatively controlled image conditions.

### `experiment_2_varied_lighting_face_distances.png`

Evaluation with greater variation between images, producing a more difficult same-person verification problem.

---

# Role of Face Recognition in the System

Face recognition is used as one possible **second authentication factor**.

The complete authentication flow is approximately:

```text
RFID
or
Employee ID + PIN
        ↓
First factor accepted
        ↓
Backend randomly selects:
FACE or FINGERPRINT
        ↓
Face image captured by ESP32
        ↓
Image sent to FastAPI backend
        ↓
InsightFace embedding generated
        ↓
Euclidean distance calculated
        ↓
Threshold decision
        ↓
Authentication accepted or rejected
```

The face model therefore does not independently determine who a person is.

The backend already knows which user is attempting authentication from the first factor.

Face recognition is used to answer:

```text
Does this face match the enrolled face
for the expected user?
```

This makes the task a **one-to-one verification problem**, rather than a one-to-many face identification problem.

---

# Face Embeddings

The system uses InsightFace with the:

```text
buffalo_l
```

model package.

For each detected face, InsightFace produces a numerical representation called a **face embedding**.

The embedding represents facial characteristics in a high-dimensional feature space.

In this project, the embedding contains:

```text
512 dimensions
```

Two photographs do not need to have identical pixel values to produce similar embeddings.

Instead, images belonging to the same person should generally be located closer together in embedding space than images belonging to different people.

---

# Normalized Embeddings

The experiments use normalized InsightFace embeddings.

Normalization makes the embeddings comparable on a consistent scale and prevents the magnitude of the vector itself from dominating the distance measurement.

Conceptually, an embedding can be represented as:

```text
e = [e1, e2, e3, ..., e512]
```

The system compares facial representations rather than raw images.

---

# Euclidean Distance

Similarity is measured using Euclidean distance.

For two embeddings:

```text
A = [a1, a2, ..., an]
B = [b1, b2, ..., bn]
```

the Euclidean distance is:

```text
d(A,B) = √Σ(ai - bi)²
```

The interpretation is:

```text
smaller distance
→ embeddings are more similar

larger distance
→ embeddings are less similar
```

For face verification:

```text
small distance
→ more evidence that the faces belong to the same person

large distance
→ more evidence that the faces belong to different people
```

The backend therefore performs a threshold decision:

```text
distance <= threshold
→ accept

distance > threshold
→ reject
```

---

# Genuine and Impostor Comparisons

The experiments divide face comparisons into two categories.

## Genuine comparisons

A genuine comparison contains two images of the **same identity**.

Example:

```text
Person A — image 1
vs.
Person A — image 2
```

The desired behavior is:

```text
LOW DISTANCE
```

because both embeddings should represent the same person.

---

## Impostor comparisons

An impostor comparison contains images from **different identities**.

Example:

```text
Person A
vs.
Person B
```

The desired behavior is:

```text
HIGH DISTANCE
```

because the embeddings represent different people.

---

# What the Experiments Are Testing

The most important question is not simply whether genuine distances are "small."

The important question is whether the two groups are separated:

```text
GENUINE DISTANCES

        gap

IMPOSTOR DISTANCES
```

If the largest genuine distances remain smaller than the smallest impostor distances, then a threshold can potentially be placed between the two populations.

For example:

```text
0.2       0.5       0.8       1.1       1.4

Genuine
████████████████

                              ███████████████
                              Impostor
```

A threshold can then be placed in the empty region between them.

---

# Experiment 1 — Controlled Conditions

![Experiment 1 — Controlled Face Distances](experiment_1_controlled_face_distances.png)

Experiment 1 evaluates InsightFace under relatively controlled image conditions.

The purpose of this experiment was to establish a baseline.

If the model could not clearly distinguish same-person and different-person comparisons under controlled conditions, it would not be appropriate to proceed to more difficult conditions.

---

## Dataset

The experiment used:

```text
249 subjects
```

For each subject, five views were selected:

```text
051
140
050
130
041
```

The selected samples used lighting condition:

```text
06
```

Using multiple views allowed the experiment to introduce pose variation while maintaining relatively consistent imaging conditions.

---

# Experiment 1 Procedure

For each selected face image:

1. the image was loaded
2. InsightFace detected the face
3. the model generated a normalized embedding
4. embeddings belonging to the same subject were compared
5. embeddings belonging to different subjects were compared
6. Euclidean distances were recorded

The comparisons were then separated into:

```text
genuine distances
impostor distances
```

---

# Number of Comparisons

The experiment generated:

```text
2,490 genuine comparisons
```

and:

```text
771,900 impostor comparisons
```

The impostor count is much larger because each identity can be compared against many other identities.

The goal was not to artificially balance the number of comparisons.

Instead, the experiment calculated the available relationships between the selected embeddings.

---

# Experiment 1 Results

## Genuine distances

```text
Minimum: 0.2734525
Maximum: 0.81328243
Average: 0.48621723
```

## Impostor distances

```text
Minimum: 1.0593255
Maximum: 1.5783218
Average: 1.3946716
```

---

# Interpreting Experiment 1

The most important numbers are:

```text
Largest genuine distance:
0.8133

Smallest impostor distance:
1.0593
```

The observed separation is therefore approximately:

```text
1.0593 - 0.8133
≈ 0.246
```

This means that, within this experiment, there was an empty region of approximately:

```text
0.246 Euclidean-distance units
```

between the hardest same-person comparison and the closest different-person comparison.

No overlap between genuine and impostor comparisons was observed.

---

# Why the Maximum Genuine Distance Matters

The average genuine distance was approximately:

```text
0.486
```

but authentication thresholds should not be selected using only the average.

A threshold near the average would reject many legitimate images that are more difficult than the typical comparison.

The more useful boundary is the upper end of the genuine distribution.

Experiment 1 shows that a legitimate same-person comparison could reach approximately:

```text
0.813
```

even under the more controlled conditions.

---

# Why the Minimum Impostor Distance Matters

The average impostor distance was:

```text
1.395
```

but this is also not the most important value for threshold selection.

The dangerous cases are not average impostors.

The dangerous cases are the **closest different-person embeddings**.

The minimum observed impostor distance was:

```text
1.059
```

This represents the different-person comparison that appeared most similar within the experiment.

The threshold should therefore remain comfortably below this region.

---

# Understanding the First Graph

The first figure shows the distance behavior of genuine and impostor comparisons.

The important feature of the graph is the separation between the two populations.

The genuine comparisons occupy the lower-distance region because they represent the same identities.

The impostor comparisons occupy the higher-distance region because they represent different identities.

Conceptually:

```text
Distance →

0.27                             0.81
|--------------------------------|
        Genuine comparisons


                                     GAP


                                           1.06                    1.58
                                           |--------------------------|
                                                 Impostor comparisons
```

This is the desired behavior for a verification system.

---

# Experiment 2 — Increased Capture Variation

![Experiment 2 — Varied Lighting Face Distances](experiment_2_varied_lighting_face_distances.png)

Experiment 1 establishes that the model performs well when image conditions remain relatively controlled.

However, an actual biometric kiosk does not operate under perfectly identical conditions.

The authentication image may differ from the enrollment images because of:

- lighting
- pose
- facial orientation
- camera position
- image quality
- distance from the camera
- minor changes in expression
- capture timing

Experiment 2 therefore increases variation between genuine samples.

---

# Purpose of Experiment 2

The second experiment asks a more important real-world question:

> What happens to the genuine distance distribution when the same person looks less similar between captures?

This matters because a threshold that performs well only under controlled conditions may reject legitimate users once deployed on physical hardware.

---

# Experiment 2 Results

The genuine comparisons became more difficult.

The observed genuine values included:

```text
Maximum genuine distance: 0.906
Average genuine distance: 0.589
```

The minimum observed impostor distance remained approximately:

```text
1.059
```

The separation therefore became:

```text
1.059 - 0.906
≈ 0.153
```

---

# Comparison With Experiment 1

The difference between the experiments is important.

## Experiment 1

```text
Average genuine:   0.486
Maximum genuine:   0.813
Minimum impostor:  1.059
Observed gap:      ~0.246
```

## Experiment 2

```text
Average genuine:   0.589
Maximum genuine:   0.906
Minimum impostor:  1.059
Observed gap:      ~0.153
```

The genuine distribution moved toward larger distances.

That is expected.

More image variation makes two photographs of the same person appear less similar to the embedding model.

---

# What the Second Graph Demonstrates

The second graph is especially important because it demonstrates the reduction in safety margin as the capture conditions become more difficult.

Conceptually:

```text
Experiment 1

Genuine
|-------------|

                     LARGE GAP

                              |----------------|
                                  Impostor
```

Compared with:

```text
Experiment 2

Genuine
|------------------|

                  SMALLER GAP

                              |----------------|
                                  Impostor
```

The model still separated the genuine and impostor comparisons in the evaluated data, but the margin was reduced.

This is more representative of the challenge expected during hardware deployment.

---

# Genuine Distribution Shift

The genuine average increased from:

```text
0.486
```

to:

```text
0.589
```

The maximum genuine distance increased from:

```text
0.813
```

to:

```text
0.906
```

This demonstrates that environmental variation primarily affected the same-person comparisons by making them less tightly clustered.

The experiment therefore supports the need for a threshold with enough tolerance to accommodate realistic changes in appearance and capture conditions.

---

# Impostor Separation

Despite the increased genuine variation, the closest observed impostor remained around:

```text
1.059
```

while the maximum genuine distance was:

```text
0.906
```

Therefore:

```text
0.906 < 1.059
```

and no genuine/impostor overlap was observed in the evaluated data.

---

# Threshold Investigation

The experiment figures include a candidate threshold around:

```text
0.93
```

This threshold was used during the experimental analysis to visualize a possible decision boundary.

The final backend configuration uses:

```text
FACE_THRESHOLD = 0.95
```

The difference is intentional.

The graphs represent the experimental threshold investigation.

The deployed backend uses a slightly more permissive threshold to provide additional tolerance for real camera captures.

---

# Why 0.95 Was Chosen

The most difficult genuine comparison observed in Experiment 2 was approximately:

```text
0.906
```

The closest observed impostor comparison was approximately:

```text
1.059
```

The deployed threshold:

```text
0.95
```

lies between these values:

```text
0.906 < 0.95 < 1.059
```

This provides approximately:

```text
0.95 - 0.906
= 0.044
```

of additional tolerance beyond the largest genuine distance observed in Experiment 2.

At the same time, the threshold remains approximately:

```text
1.059 - 0.95
= 0.109
```

below the closest observed impostor comparison.

Conceptually:

```text
largest observed genuine
        0.906

           ↓

-----------|----|----------------|-----------

               0.95            1.059
            threshold       closest impostor
```

The threshold therefore represents a tradeoff.

Increasing the threshold provides more tolerance for legitimate variation but also moves the decision boundary closer to impostor comparisons.

Decreasing the threshold provides more separation from impostors but increases the chance of rejecting legitimate users.

---

# Threshold Is Project-Specific

The value:

```text
0.95
```

should not be interpreted as a universal InsightFace threshold.

Threshold behavior depends on factors such as:

- model
- preprocessing
- embedding representation
- distance metric
- camera
- dataset
- enrollment procedure
- environmental conditions

The threshold was selected for this project using the observed development experiments.

A production deployment would require substantially broader validation.

---

# False Acceptance and False Rejection

Threshold selection represents a balance between two types of errors.

## False rejection

A legitimate user is rejected.

This can happen if:

```text
genuine distance > threshold
```

A threshold that is too strict increases this risk.

---

## False acceptance

A different person is incorrectly accepted.

This could happen if:

```text
impostor distance <= threshold
```

A threshold that is too permissive increases this risk.

---

# What These Experiments Do and Do Not Measure

The experiments demonstrate the observed separation between genuine and impostor distances.

They do **not** constitute a complete biometric benchmark.

The project did not claim formal production values for metrics such as:

```text
FAR
False Acceptance Rate

FRR
False Rejection Rate

EER
Equal Error Rate
```

The experimental results instead provide evidence that the selected embedding representation and distance metric produced useful separation for the evaluated data.

---

# Experiment Summary

| Experiment | Genuine Minimum | Genuine Average | Genuine Maximum | Impostor Minimum | Impostor Average | Impostor Maximum | Observed Gap |
|---|---:|---:|---:|---:|---:|---:|---:|
| Controlled conditions | 0.273 | 0.486 | 0.813 | 1.059 | 1.395 | 1.578 | ~0.246 |
| Increased variation | — | 0.589 | 0.906 | ~1.059 | — | — | ~0.153 |

Values that were not retained from the second experiment are intentionally not filled with estimates.

---

# Multi-Image Enrollment

The final system does not enroll a user using only one photograph.

Instead, enrollment requires five face captures.

The prompts are:

```text
LOOK_STRAIGHT
TURN_SLIGHTLY_LEFT
TURN_MORE_LEFT
TURN_SLIGHTLY_RIGHT
TURN_MORE_RIGHT
```

The purpose is to capture more variation in the user's appearance.

---

# Enrollment Embedding Construction

Each successful enrollment capture produces a normalized face embedding.

Conceptually:

```text
Capture 1 → embedding E1
Capture 2 → embedding E2
Capture 3 → embedding E3
Capture 4 → embedding E4
Capture 5 → embedding E5
```

The backend combines the five embeddings by averaging them:

```text
Enrollment embedding
=
(E1 + E2 + E3 + E4 + E5) / 5
```

This produces a representation based on multiple views rather than a single photograph.

The motivation is to reduce sensitivity to one particular pose.

---

# Why Multiple Head Positions Are Used

A single frontal photograph may produce an excellent match when the authentication image is also frontal.

However, a kiosk user will not reproduce the exact same pose every time.

The five-capture process intentionally includes:

```text
center
left variation
additional left variation
right variation
additional right variation
```

This allows the stored enrollment representation to incorporate information from multiple orientations.

---

# Enrollment Pipeline

The deployed enrollment pipeline is approximately:

```text
ESP32 camera
     ↓
capture image
     ↓
HTTP upload
     ↓
FastAPI
     ↓
OpenCV decoding
     ↓
InsightFace detection
     ↓
512-dimensional normalized embedding
     ↓
repeat for five prompted positions
     ↓
average embeddings
     ↓
store enrolled representation
     ↓
PostgreSQL
```

---

# Authentication Pipeline

Authentication uses a new camera image.

```text
ESP32 camera
     ↓
capture authentication image
     ↓
HTTP upload
     ↓
FastAPI
     ↓
OpenCV decoding
     ↓
InsightFace face detection
     ↓
normalized embedding
     ↓
load enrolled embedding
     ↓
Euclidean distance
     ↓
compare with 0.95
     ↓
ACCEPT / REJECT
```

---

# Relationship Between the Experiments and the Deployed System

The offline experiments were used to understand the behavior of the embedding model before hardware integration.

They established that:

1. same-person embeddings generally produced smaller distances
2. different-person embeddings generally produced substantially larger distances
3. increased capture variation pushed genuine distances upward
4. the genuine and impostor distributions remained separated in the evaluated experiments
5. a verification threshold around the region between the two populations was reasonable for further system testing

The final system then applied this analysis to the real backend.

---

# Hardware Validation

Offline dataset evaluation is not enough for an embedded access-control system.

The actual camera introduces additional variables.

The final system therefore uses captures from the ESP32 camera during real enrollment and authentication.

The hardware integration validates the complete path:

```text
physical person
      ↓
ESP32 camera
      ↓
captured image
      ↓
network request
      ↓
FastAPI
      ↓
OpenCV
      ↓
InsightFace
      ↓
embedding
      ↓
distance comparison
      ↓
authentication state machine
```

This is important because an ML model that performs well on stored dataset images may behave differently with images produced by the actual embedded camera.

---

# ESP32 Camera Considerations

The camera used by the physical kiosk has different characteristics from the images used in offline experiments.

Possible sources of variation include:

- lower image quality
- compression
- sensor noise
- exposure
- field of view
- cropping
- camera placement
- subject distance
- environmental lighting

For this reason, the physical demonstration enrolls a consenting participant using actual ESP32 captures rather than relying on the offline test images.

---

# Why Offline and Hardware Testing Are Both Necessary

The two forms of evaluation answer different questions.

## Offline experiments

Answer:

```text
Does the embedding model show useful
genuine/impostor separation?
```

## Hardware integration testing

Answers:

```text
Does the complete deployed pipeline work
with the actual kiosk camera and network path?
```

Both are necessary for a meaningful system demonstration.

---

# What the Graphs Should Not Be Interpreted As

The figures should not be interpreted as:

```text
model accuracy graphs
training curves
loss curves
neural-network training results
```

No face-recognition network was trained as part of this project.

The graphs represent:

```text
distributions of embedding distances
```

for genuine and impostor comparisons.

They evaluate the behavior of an existing pretrained model within the project's verification pipeline.

---

# Why No Model Training Was Required

The goal of the project was to build an integrated biometric access-control system rather than develop a new face-recognition architecture.

Using a pretrained model allowed the work to focus on:

- embedding evaluation
- threshold analysis
- authentication architecture
- enrollment design
- backend integration
- embedded-camera integration
- multi-factor authentication
- cloud deployment

This reflects the role of machine learning as one component within a larger engineering system.

---

# Security Interpretation

A face match should be interpreted carefully.

The face-recognition system determines whether:

```text
submitted face embedding
≈
stored enrolled face embedding
```

It does not independently prove:

- physical presence
- liveness
- resistance to presentation attacks
- that the person is not using a photograph or display

---

# Liveness Detection

Dedicated liveness detection or anti-spoofing is not currently implemented.

The system should therefore be described as:

```text
face recognition
```

or:

```text
face verification
```

not as:

```text
liveness detection
```

This distinction is deliberate.

---

# Why Face Recognition Is Not the Only Factor

Face recognition is not used by itself.

The system first establishes identity using:

```text
RFID
```

or the fallback:

```text
employee ID + PIN
```

The backend then requests:

```text
FACE
```

or:

```text
FINGERPRINT
```

as the second factor.

This means the face model operates as one component of a multi-factor authentication system.

---

# Experimental Conclusions

The evaluation produced several useful findings.

## 1. Strong separation under controlled conditions

Experiment 1 produced:

```text
maximum genuine = 0.813
minimum impostor = 1.059
```

giving an observed gap of approximately:

```text
0.246
```

---

## 2. Capture variation affects genuine similarity

Experiment 2 increased the maximum genuine distance to:

```text
0.906
```

and the genuine average to:

```text
0.589
```

This confirms that environmental and image variation matters.

---

## 3. Separation remained in the evaluated data

Even after increased variation:

```text
maximum genuine ≈ 0.906

minimum impostor ≈ 1.059
```

leaving approximately:

```text
0.153
```

of observed separation.

---

## 4. Threshold selection requires a compromise

The final threshold:

```text
0.95
```

was selected above the most difficult genuine comparison observed during these experiments while remaining below the closest observed impostor comparison.

---

## 5. Hardware validation remains important

Offline evaluation cannot fully represent the characteristics of the ESP32 camera.

The final system therefore performs enrollment and authentication using the actual camera used by the kiosk.

---

# Limitations

The experiments should be interpreted within the scope of a prototype engineering project.

Important limitations include:

- only 249 identities were included in Experiment 1
- the evaluation dataset does not represent every possible user population
- only selected capture conditions were tested
- ESP32 camera images may differ from the evaluation images
- dedicated liveness detection was not implemented
- formal FAR, FRR, and EER benchmarking was not performed
- no claim is made that 0.95 is a universal InsightFace threshold
- deployment environments may introduce additional image variation

A production biometric access-control system would require broader testing across substantially more users, devices, lighting conditions, demographic groups, and attack scenarios.

---

# Future ML Improvements

Possible future extensions include:

- larger evaluation datasets
- formal FAR measurement
- formal FRR measurement
- Equal Error Rate analysis
- ROC curve generation
- threshold calibration using real ESP32 captures
- evaluation across multiple cameras
- explicit image-quality checks
- face alignment validation
- liveness detection
- presentation-attack detection
- testing under extreme lighting
- evaluation of glasses, facial hair, and appearance changes

These features are outside the scope of the current prototype but provide clear directions for future development.

---

# Final Interpretation

The ML component was not treated as a black-box API.

The project experimentally evaluated the distance behavior of the chosen face-embedding model before incorporating it into the access-control system.

The experiments demonstrated that, for the evaluated data:

```text
same-person comparisons
<
different-person comparisons
```

with no observed genuine/impostor overlap in either experiment.

Increasing capture variation reduced the separation margin, demonstrating why real-world conditions must be considered when selecting a verification threshold.

The resulting analysis informed the final backend threshold and the multi-image enrollment strategy used by the physical biometric kiosk.

---
