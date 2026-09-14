const form = document.getElementById("add-user-form");
const resultDiv = document.getElementById("result");
let editingUserId = null;
const adminToken = localStorage.getItem("admin_token");
if (!adminToken) {
    window.location.href = "login.html";
}
// Adds the admin JWT to protected API requests
async function adminFetch(url, options = {}) {
    const headers = {
        ...(options.headers || {}),
        "Authorization": `Bearer ${adminToken}`
    };
    const response = await fetch(url, {
        ...options,
        headers: headers
    });
    if (response.status === 401) {
        localStorage.removeItem("admin_token");
        alert("Your admin session has expired. Please log in again.");
        window.location.href = "login.html";
        return null;
    }
    return response;
}

function logout() {
    localStorage.removeItem("admin_token");
    window.location.href = "login.html";
}

form.addEventListener("submit", async function(event) {
    event.preventDefault();
    const firstName = document.getElementById("first-name").value.trim();
    const lastName = document.getElementById("last-name").value.trim();
    const pin = document.getElementById("pin").value;
    const response = await adminFetch(
        "http://127.0.0.1:8000/admin/users",
        {
            method: "POST",
            headers: {
                "Content-Type": "application/json"
            },
            body: JSON.stringify({
                first_name: firstName,
                last_name: lastName,
                pin: pin
            })
        }
    );
    if (!response) {
        return;
    }
    const data = await response.json();
    if (data.success) {
        resultDiv.innerHTML = `
            <p>User created successfully.</p>
            <p>Employee ID: ${data.employee_id}</p>
            <p>Status: ${data.status}</p>
        `;
        form.reset();
        loadUsers();
    } else {
        resultDiv.innerHTML = `
            <p>Error: ${data.reason}</p>
        `;
    }
});

async function loadUsers() {
    const response = await adminFetch("http://127.0.0.1:8000/admin/users");
    if (!response) {
        return;
    }
    const data = await response.json();
    const usersBody = document.getElementById("users-body");
    usersBody.innerHTML = "";
    if (!data.success || !Array.isArray(data.users)) {
        alert(
            `Failed to load users: ${
                data.reason ??
                data.detail ??
                "UNKNOWN_ERROR"
            }`
        );
        return;
    }
    for (const user of data.users) {
        const row = document.createElement("tr");
        row.innerHTML = `
            <td>${user.user_id}</td>
            <td>${user.employee_id}</td>
            <td>${user.first_name} ${user.last_name}</td>
            <td>${user.status}</td>

            <td class="actions-cell">
                <button
                    type="button"
                    class="menu-button"
                    onclick="toggleMenu(${user.user_id})"
                >
                    ⋮
                </button>

                <div class="user-menu" id="menu-${user.user_id}">
                    <button
                        type="button"
                        onclick="editUser(${user.user_id})"
                    >
                        Edit User
                    </button>

                    <button
                        type="button"
                        onclick="reenrollFace(${user.user_id})"
                    >
                        Re-enroll Face
                    </button>

                    <button
                        type="button"
                        onclick="reenrollRFID(${user.user_id})"
                    >
                        Replace RFID
                    </button>

                    <button
                        type="button"
                        onclick="reenrollFingerprint(${user.user_id})"
                    >
                        Re-enroll Fingerprints
                    </button>

                    <button
                        type="button"
                        onclick="resetUserCredentials(${user.user_id})"
                    >
                        Reset Credentials
                    </button>

                    ${getStatusAction(user)}
                </div>
            </td>
        `;

        usersBody.appendChild(row);
    }
}

function toggleMenu(userId) {
    const clickedMenu = document.getElementById(`menu-${userId}`);
    const allMenus = document.querySelectorAll(".user-menu");
    for (const menu of allMenus) {
        if (menu !== clickedMenu) {
            menu.classList.remove("show");
        }
    }
    clickedMenu.classList.toggle("show");
}

function getStatusAction(user) {
    if (user.status === "ACTIVE") {
        return `
            <button
                type="button"
                onclick="toggleUserStatus(${user.user_id}, '${user.status}')"
            >
                Deactivate
            </button>
        `;
    }
    if (user.status === "INACTIVE") {
        return `
            <button
                type="button"
                onclick="toggleUserStatus(${user.user_id}, '${user.status}')"
            >
                Activate
            </button>
        `;
    }
    return "";
}

async function toggleUserStatus(userId, currentStatus) {
    const newStatus = currentStatus === "ACTIVE" ? "INACTIVE" : "ACTIVE";
    const response = await adminFetch(
        `http://127.0.0.1:8000/admin/users/${userId}/status`,
        {
            method: "PATCH",
            headers: {
                "Content-Type": "application/json"
            },
            body: JSON.stringify({
                status: newStatus
            })
        }
    );
    if (!response) {
        return;
    }
    const data = await response.json();
    if (data.success) {
        loadUsers();
    } else {
        alert(`Status change failed: ${data.reason}`);
    }
}

async function editUser(userId) {
    const response = await adminFetch("http://127.0.0.1:8000/admin/users");
    if (!response) {
        return;
    }
    const data = await response.json();
    const user = data.users.find(user => user.user_id === userId);
    if (!user) {
        return;
    }
    editingUserId = userId;
    document.getElementById("edit-first-name").value = user.first_name;
    document.getElementById("edit-last-name").value = user.last_name;
    document.getElementById("edit-modal").classList.add("show");
}

function closeEditModal() {
    document.getElementById("edit-modal").classList.remove("show");
    editingUserId = null;
}

async function saveUserEdit() {
    const firstName = document.getElementById("edit-first-name").value.trim();
    const lastName = document.getElementById("edit-last-name").value.trim();
    if (!firstName || !lastName) {
        alert("First name and last name are required.");
        return;
    }
    const response = await adminFetch(
        `http://127.0.0.1:8000/admin/users/${editingUserId}`,
        {
            method: "PATCH",
            headers: {
                "Content-Type": "application/json"
            },
            body: JSON.stringify({
                first_name: firstName,
                last_name: lastName
            })
        }
    );
    if (!response) {
        return;
    }
    const data = await response.json();
    if (data.success) {
        closeEditModal();
        loadUsers();
    } else {
        alert(`Edit failed: ${data.reason}`);
    }
}

document.getElementById("employee-search").addEventListener("input", function() {
    const searchValue = this.value.trim().toLowerCase();
    const rows = document.querySelectorAll("#users-body tr");
    for (const row of rows) {
        // Column 1 contains the employee ID
        const employeeId = row.children[1].textContent.trim().toLowerCase();
        row.style.display = employeeId.includes(searchValue) ? "" : "none";
    }
});

async function resetUserCredentials(userId) {
    const confirmed = confirm(
        "Are you sure you want to reset this user's credentials? " +
        "They will need to enroll again."
    );
    if (!confirmed) {
        return;
    }
    const response = await adminFetch(
        `http://127.0.0.1:8000/admin/users/${userId}/reset-credentials`,
        {
            method: "POST"
        }
    );
    if (!response) {
        return;
    }
    const data = await response.json();
    if (data.success) {
        let message =
            "Credentials reset successfully.\n" +
            "User status is now PENDING_ENROLLMENT.";
        if (
            data.fingerprint_slots_to_delete &&
            data.fingerprint_slots_to_delete.length > 0
        ) {
            message +=
                "\nFingerprint slots to delete from sensor: " +
                data.fingerprint_slots_to_delete.join(", ");
        }
        alert(message);
        loadUsers();
    } else {
        alert(`Reset failed: ${data.reason}`);
    }
}

async function reenrollFace(userId) {
    const confirmed = confirm("Start face re-enrollment for this user?");
    if (!confirmed) {
        return;
    }
    const response = await adminFetch(
        `http://127.0.0.1:8000/admin/users/${userId}/face/reenroll`,
        {
            method: "POST"
        }
    );
    if (!response) {
        return;
    }
    const data = await response.json();
    if (data.success) {
        alert(
            "Face re-enrollment requested. " +
            "The user can complete it at the kiosk."
        );
    } else {
        alert(`Failed: ${data.reason}`);
    }
}

async function reenrollRFID(userId) {
    const confirmed = confirm("Start RFID replacement for this user?");
    if (!confirmed) {
        return;
    }
    const response = await adminFetch(
        `http://127.0.0.1:8000/admin/users/${userId}/rfid/reenroll`,
        {
            method: "POST"
        }
    );
    if (!response) {
        return;
    }
    const data = await response.json();
    if (data.success) {
        alert(
            "RFID replacement requested. " +
            "The user can complete it at the kiosk."
        );
    } else {
        alert(`Failed: ${data.reason}`);
    }
}

async function reenrollFingerprint(userId) {
    const confirmed = confirm(
        "Start fingerprint re-enrollment for this user?"
    );
    if (!confirmed) {
        return;
    }
    const response = await adminFetch(
        `http://127.0.0.1:8000/admin/users/${userId}/fingerprints/reenroll`,
        {
            method: "POST"
        }
    );
    if (!response) {
        return;
    }
    const data = await response.json();
    if (data.success) {
        alert(
            "Fingerprint re-enrollment requested. " +
            "The user can complete it at the kiosk."
        );
    } else {
        alert(`Failed: ${data.reason}`);
    }
}

loadUsers();